/**
  ******************************************************************************
  * @file    camera.c
  * @brief   DCMI + DMA2 capture path for the OV5640.
  ******************************************************************************
  */

#include "camera.h"
#include "ov5640.h"
#include <string.h>

/* MX_DCMI_Init() owns this handle; it lives in main.c. */
extern DCMI_HandleTypeDef hdcmi;

/* -------------------------------------------------------------------------- */
/* Frame buffer                                                               */
/* -------------------------------------------------------------------------- */

/*
 * Two 320 x 240 x 2 = 153,600 byte buffers for ping-pong live preview: the
 * DCMI fills one while SPI DMA sends the other to the LCD.  They live in
 * .framebuf (SRAM1), which MPU region 1 marks non-cacheable, so neither DMA
 * ever sees stale cache lines and no cache maintenance is needed.  The CPU
 * never touches pixel data on the live path, so caching would buy nothing.
 *
 * Each buffer carries CAMERA_LIVE_SLACK spare bytes past the frame.  In live
 * mode the DMA is armed for slightly more than a frame and re-armed at every
 * end-of-frame, so a sensor that sends a few extra words spills into the
 * slack instead of the next buffer, and one that sends a few too few does
 * not leave the DMA out of step with frame boundaries.
 *
 * Snapshot captures (the UART debug path) use buffer 0.
 */
#define CAMERA_LIVE_SLACK       (OV5640_FRAME_WIDTH * OV5640_BYTES_PER_PIXEL * 2U)
#define CAMERA_BUFFER_BYTES     (OV5640_FRAME_SIZE_BYTES + CAMERA_LIVE_SLACK)

static uint8_t camera_frame_buffers[2][CAMERA_BUFFER_BYTES]
    __attribute__((aligned(32), section(".framebuf")));

#define camera_frame_buffer     (camera_frame_buffers[0])

/* DCMI moves data as 32-bit words, so the DMA length is in words, not bytes. */
#define CAMERA_DMA_WORD_COUNT   (OV5640_FRAME_SIZE_BYTES / 4U)
#define CAMERA_LIVE_WORD_COUNT  (CAMERA_BUFFER_BYTES / 4U)

/*
 * How much of the tail may be missing and still count as a good frame.
 *
 * The sensor reliably delivers a few words fewer than width*height*2 - the
 * DCMI does not forward the last handful of pixel clocks before VSYNC - so
 * requiring an exact DMA completion rejects otherwise perfect frames over
 * eight or twenty bytes.  1% is far tighter than a genuinely truncated
 * frame, which comes up short by a third or more.
 */
#define CAMERA_MAX_MISSING_WORDS  (CAMERA_DMA_WORD_COUNT / 100U)

/* How many times to re-arm before giving up on getting a whole frame. */
#define CAMERA_CAPTURE_ATTEMPTS   12U

/* -------------------------------------------------------------------------- */
/* State                                                                      */
/* -------------------------------------------------------------------------- */

static DMA_HandleTypeDef hdma_dcmi;

static volatile bool     camera_frame_ready;
static volatile uint32_t camera_overrun_count;
static volatile uint32_t camera_sync_error_count;

/* How far the DMA got when a capture timed out, in 32-bit words still
 * outstanding.  Near zero means the frame almost completed (a few missing
 * pixel clocks); near CAMERA_DMA_WORD_COUNT means nothing arrived at all. */
static uint32_t camera_timeout_remaining;

/* Milliseconds the last successful capture took, start to end-of-frame. */
static uint32_t camera_last_frame_ms;

/* Whether the frame just returned lost any data to a FIFO overrun. */
static bool camera_last_frame_overrun;

/* Arming attempts the last Camera_Capture() needed to get a whole frame. */
static uint32_t camera_attempts;

/* DCMI register snapshot taken at the moment a capture gave up, so a
 * timeout can be attributed rather than guessed at. */
static uint32_t camera_dbg_cr;
static uint32_t camera_dbg_sr;
static uint32_t camera_dbg_risr;
static uint32_t camera_dbg_misr;
static volatile uint32_t camera_frame_events;
static volatile uint32_t camera_vsync_events;

/* -------------------------------------------------------------------------- */
/* MSP: DMA2 Stream1 Channel1 is the DCMI request                             */
/* -------------------------------------------------------------------------- */

void Camera_MspDmaInit(DCMI_HandleTypeDef *hdcmi_handle)
{
  __HAL_RCC_DMA2_CLK_ENABLE();

  hdma_dcmi.Instance                 = DMA2_Stream1;
  hdma_dcmi.Init.Channel             = DMA_CHANNEL_1;
  hdma_dcmi.Init.Direction           = DMA_PERIPH_TO_MEMORY;
  hdma_dcmi.Init.PeriphInc           = DMA_PINC_DISABLE;
  hdma_dcmi.Init.MemInc              = DMA_MINC_ENABLE;
  hdma_dcmi.Init.PeriphDataAlignment = DMA_PDATAALIGN_WORD;
  hdma_dcmi.Init.MemDataAlignment    = DMA_MDATAALIGN_WORD;
  hdma_dcmi.Init.Mode                = DMA_NORMAL;
  hdma_dcmi.Init.Priority            = DMA_PRIORITY_HIGH;

  /* The DCMI DR is drained four words at a time into an INC4 memory burst;
   * this is what keeps the 4-word DCMI FIFO from overrunning. */
  hdma_dcmi.Init.FIFOMode            = DMA_FIFOMODE_ENABLE;
  hdma_dcmi.Init.FIFOThreshold       = DMA_FIFO_THRESHOLD_FULL;
  hdma_dcmi.Init.MemBurst            = DMA_MBURST_INC4;
  hdma_dcmi.Init.PeriphBurst         = DMA_PBURST_SINGLE;

  if (HAL_DMA_Init(&hdma_dcmi) != HAL_OK)
  {
    Error_Handler();
  }

  __HAL_LINKDMA(hdcmi_handle, DMA_Handle, hdma_dcmi);

  /* DMA above DCMI: the transfer-complete callback must be serviced before
   * the end-of-frame interrupt that follows it. */
  HAL_NVIC_SetPriority(DMA2_Stream1_IRQn, 5, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream1_IRQn);

  HAL_NVIC_SetPriority(DCMI_IRQn, 6, 0);
  HAL_NVIC_EnableIRQ(DCMI_IRQn);
}

void Camera_MspDmaDeInit(DCMI_HandleTypeDef *hdcmi_handle)
{
  HAL_NVIC_DisableIRQ(DCMI_IRQn);
  HAL_NVIC_DisableIRQ(DMA2_Stream1_IRQn);

  if (hdcmi_handle->DMA_Handle != NULL)
  {
    HAL_DMA_DeInit(hdcmi_handle->DMA_Handle);
  }
}

void Camera_DmaIrqHandler(void)
{
  HAL_DMA_IRQHandler(&hdma_dcmi);
}

/* -------------------------------------------------------------------------- */
/* Init                                                                       */
/* -------------------------------------------------------------------------- */

void Camera_EnableXclk(void)
{
#if OV5640_DRIVE_XCLK_FROM_MCO1
  /* HAL_RCC_MCOConfig also configures PA8 as AF0 push-pull for us.  Must
   * happen before the sensor is brought out of reset, and before any SCCB
   * traffic - the OV5640's serial interface is clocked from XCLK. */
  HAL_RCC_MCOConfig(RCC_MCO1, RCC_MCO1SOURCE_HSI, RCC_MCODIV_1);
  HAL_Delay(5U);
#endif
}

Camera_Status_t Camera_Init(void)
{
  camera_frame_ready      = false;
  camera_overrun_count    = 0U;
  camera_sync_error_count = 0U;

  memset(camera_frame_buffers, 0, sizeof(camera_frame_buffers));

  Camera_EnableXclk();

  switch (OV5640_Init())
  {
    case OV5640_OK:          break;
    case OV5640_ERR_I2C:     return CAMERA_ERR_I2C;
    case OV5640_ERR_CHIP_ID: return CAMERA_ERR_CHIP_ID;
    default:                 return CAMERA_ERR_CONFIG;
  }

  return CAMERA_OK;
}

/* -------------------------------------------------------------------------- */
/* Capture                                                                    */
/* -------------------------------------------------------------------------- */

/** Map the DCMI handle's error code onto our status enum. */
static Camera_Status_t Camera_TranslateError(void)
{
  if ((hdcmi.ErrorCode & HAL_DCMI_ERROR_SYNC) != 0U)
  {
    return CAMERA_ERR_SYNC;
  }

  if ((hdcmi.ErrorCode & HAL_DCMI_ERROR_OVR) != 0U)
  {
    return CAMERA_ERR_OVERRUN;
  }

  return CAMERA_ERR_DMA;
}

bool Camera_LastFrameHadOverrun(void)
{
  return camera_last_frame_overrun;
}

/**
  * @brief  Block until the sensor is in the vertical blanking gap.
  * @note   DCMI_SR bit 1 reads the VSYNC pin with the configured polarity:
  *         0 = active frame, 1 = between frames.  We wait for an active
  *         frame first and only then for the gap, so that we return at the
  *         *start* of a blanking period with a full gap ahead of us rather
  *         than at the tail end of one.
  * @retval false on timeout.
  */
static bool Camera_WaitForFrameGap(uint32_t timeout_ms)
{
  uint32_t start = HAL_GetTick();

  /* SR only tracks the pins while the interface is enabled. */
  __HAL_DCMI_ENABLE(&hdcmi);

  while ((hdcmi.Instance->SR & DCMI_SR_VSYNC) != 0U)
  {
    if ((HAL_GetTick() - start) > timeout_ms) { return false; }
  }

  while ((hdcmi.Instance->SR & DCMI_SR_VSYNC) == 0U)
  {
    if ((HAL_GetTick() - start) > timeout_ms) { return false; }
  }

  return true;
}

static Camera_Status_t Camera_CaptureOnce(uint32_t timeout_ms)
{
  uint32_t start;

  camera_frame_ready       = false;
  camera_last_frame_overrun = false;
  hdcmi.ErrorCode          = HAL_DCMI_ERROR_NONE;

  /* Arm inside the blanking gap.  Enabling the DCMI partway through a frame
   * captures a partial one: the DMA transfer count never reaches zero, so
   * the end-of-frame interrupt never fires and the capture times out.  The
   * gap here is ~500 blanked lines, far longer than the arming takes. */
  if (!Camera_WaitForFrameGap(timeout_ms))
  {
    camera_timeout_remaining = CAMERA_DMA_WORD_COUNT;
    Camera_Stop();
    return CAMERA_ERR_TIMEOUT;
  }

  if (HAL_DCMI_Start_DMA(&hdcmi, DCMI_MODE_SNAPSHOT,
                         (uint32_t)camera_frame_buffer,
                         CAMERA_DMA_WORD_COUNT) != HAL_OK)
  {
    return CAMERA_ERR_DMA;
  }

  /*
   * HAL only enables the end-of-frame interrupt from inside its DMA
   * transfer-complete callback, so a frame that arrives a few words short
   * never reports completion at all.  Enable it ourselves, up front, and let
   * the sensor's own end-of-frame tell us when the frame is done.
   *
   * The FRAME handler also disables ERR and OVR on the way out in snapshot
   * mode, so those are re-armed here for each capture.
   */
  __HAL_DCMI_CLEAR_FLAG(&hdcmi, DCMI_FLAG_FRAMERI | DCMI_FLAG_OVRRI
                                | DCMI_FLAG_ERRRI);

  /*
   * The overrun interrupt is deliberately left disabled.  HAL's handler
   * responds to an overrun by aborting the DMA outright, which destroys a
   * frame that is typically already complete - this sensor emits slightly
   * more data per frame than width*height*2, so the FIFO overflows *after*
   * the last word we wanted has already landed in memory.
   *
   * The raw OVR flag is still readable afterwards, so nothing is hidden:
   * it is checked once the frame ends and reported to the caller.
   */
  __HAL_DCMI_DISABLE_IT(&hdcmi, DCMI_IT_OVR);

  /* VSYNC is enabled purely as a diagnostic: counting the edges the DCMI
   * actually sees distinguishes "the sensor is not producing frame syncs"
   * from "the syncs arrive but end-of-frame never fires". */
  __HAL_DCMI_ENABLE_IT(&hdcmi, DCMI_IT_FRAME | DCMI_IT_ERR | DCMI_IT_VSYNC);

  start = HAL_GetTick();

  while (!camera_frame_ready)
  {
    /*
     * Poll ErrorCode, not State: on an error the IRQ handler sets State to
     * ERROR but the DMA abort callback that follows puts it straight back to
     * READY, so State is only briefly observable.  ErrorCode accumulates and
     * is only ever cleared by us.
     *
     * An overrun is deliberately NOT fatal here.  It usually lands in the
     * last few words of a frame, costing a handful of pixels, and abandoning
     * an otherwise complete frame over that is worse than keeping it.  It is
     * recorded and reported instead.  A sync error is different: it means
     * the frame structure itself is wrong, so there is nothing to salvage.
     */
    if (hdcmi.ErrorCode != HAL_DCMI_ERROR_NONE)
    {
      Camera_Stop();
      return Camera_TranslateError();
    }

    if ((HAL_GetTick() - start) > timeout_ms)
    {
      /* Sample everything before stopping - HAL_DCMI_Stop aborts the stream
       * and clears the state we need to explain the failure. */
      camera_timeout_remaining = __HAL_DMA_GET_COUNTER(hdcmi.DMA_Handle);
      camera_dbg_cr   = hdcmi.Instance->CR;
      camera_dbg_sr   = hdcmi.Instance->SR;
      camera_dbg_risr = hdcmi.Instance->RISR;
      camera_dbg_misr = hdcmi.Instance->MISR;

      Camera_Stop();
      return CAMERA_ERR_TIMEOUT;
    }
  }

  camera_last_frame_ms = HAL_GetTick() - start;
  camera_frame_ready   = false;

  /* Read the raw overrun flag straight out of RISR.  With the OVR interrupt
   * disabled it never reaches hdcmi.ErrorCode, and __HAL_DCMI_GET_FLAG is
   * unusable here: this HAL version expands it to Instance->RIS / ->MIS,
   * which do not exist - the CMSIS header calls them RISR and MISR. */
  camera_last_frame_overrun =
      ((hdcmi.Instance->RISR & DCMI_RIS_OVR_RIS) != 0U);

  if (camera_last_frame_overrun)
  {
    camera_overrun_count++;
    hdcmi.Instance->ICR = DCMI_ICR_OVR_ISC;
  }

  /* Sample how much of the tail the DMA missed before tearing the transfer
   * down - Camera_Stop aborts the stream and the counter goes with it. */
  camera_timeout_remaining = __HAL_DMA_GET_COUNTER(hdcmi.DMA_Handle);

  Camera_Stop();
  __HAL_DCMI_DISABLE(&hdcmi);

  /* A frame short by more than the tolerance means the DCMI was armed
   * partway through one and only caught the tail.  Distinct from a timeout:
   * end-of-frame did arrive, there just was not a whole frame behind it. */
  if (camera_timeout_remaining > CAMERA_MAX_MISSING_WORDS)
  {
    camera_dbg_cr   = hdcmi.Instance->CR;
    camera_dbg_sr   = hdcmi.Instance->SR;
    camera_dbg_risr = hdcmi.Instance->RISR;
    camera_dbg_misr = hdcmi.Instance->MISR;
    return CAMERA_ERR_PARTIAL;
  }

  /* Blank whatever the DMA did not reach so the tail of the frame is not
   * left holding pixels from the previous capture. */
  if (camera_timeout_remaining > 0U)
  {
    uint32_t written = (CAMERA_DMA_WORD_COUNT - camera_timeout_remaining) * 4U;
    memset(&camera_frame_buffer[written], 0,
           OV5640_FRAME_SIZE_BYTES - written);
  }

  return CAMERA_OK;
}

Camera_Status_t Camera_Capture(uint32_t timeout_ms)
{
  uint32_t        deadline = HAL_GetTick() + timeout_ms;
  Camera_Status_t status   = CAMERA_ERR_TIMEOUT;

  /*
   * Retry on partial frames.
   *
   * Arming the DCMI lands somewhere in the sensor's frame timing, and
   * despite waiting for the blanking gap it sometimes still catches a frame
   * already in progress and captures only its tail.  Complete frames do come
   * through, so the useful behaviour is to try again rather than hand the
   * caller a torn image.  Hard faults - sync errors, DMA setup failures -
   * will not fix themselves and return immediately.
   */
  for (uint32_t attempt = 0U; attempt < CAMERA_CAPTURE_ATTEMPTS; attempt++)
  {
    uint32_t now = HAL_GetTick();
    uint32_t remaining;

    if (now >= deadline)
    {
      break;
    }

    remaining = deadline - now;
    camera_attempts = attempt + 1U;

    status = Camera_CaptureOnce(remaining < 500U ? remaining : 500U);

    if (status == CAMERA_OK)
    {
      return CAMERA_OK;
    }

    if ((status == CAMERA_ERR_SYNC) || (status == CAMERA_ERR_DMA))
    {
      return status;
    }
  }

  return status;
}

uint32_t Camera_GetLastAttempts(void)
{
  return camera_attempts;
}

Camera_Status_t Camera_StartContinuous(void)
{
  camera_frame_ready = false;
  hdcmi.ErrorCode    = HAL_DCMI_ERROR_NONE;

  if (HAL_DCMI_Start_DMA(&hdcmi, DCMI_MODE_CONTINUOUS,
                         (uint32_t)camera_frame_buffer,
                         CAMERA_DMA_WORD_COUNT) != HAL_OK)
  {
    return CAMERA_ERR_DMA;
  }

  return CAMERA_OK;
}

void Camera_Stop(void)
{
  (void)HAL_DCMI_Stop(&hdcmi);
}

bool Camera_IsFrameReady(void)
{
  bool ready = camera_frame_ready;

  camera_frame_ready = false;

  return ready;
}

const uint8_t *Camera_GetBuffer(void)
{
  return camera_frame_buffer;
}

uint32_t Camera_GetBufferSize(void)
{
  return OV5640_FRAME_SIZE_BYTES;
}

uint32_t Camera_GetOverrunCount(void)
{
  return camera_overrun_count;
}

uint32_t Camera_GetTimeoutRemainingWords(void)
{
  return camera_timeout_remaining;
}

uint32_t Camera_GetExpectedWords(void)
{
  return CAMERA_DMA_WORD_COUNT;
}

uint32_t Camera_GetLastFrameMs(void)
{
  return camera_last_frame_ms;
}

uint8_t *Camera_GetFrameBuffer(uint32_t index)
{
  return camera_frame_buffers[index & 1U];
}

/* -------------------------------------------------------------------------- */
/* Live preview: continuous capture, ping-pong between the two buffers        */
/* -------------------------------------------------------------------------- */

/*
 * HAL's continuous mode is not used here.  It arms the DMA once and lets it
 * run, so the buffer boundary only lines up with the frame boundary if every
 * frame is exactly width*height*2 bytes - and this sensor is not exact.
 * Instead the DCMI end-of-frame interrupt stops the stream, hands the
 * finished buffer over, and re-arms into the other one.  That happens at the
 * start of vertical blanking, hundreds of lines before the next frame.
 *
 * Buffer ownership, all indices 0/1 or -1:
 *   live_cap   - DMA is writing into it
 *   live_ready - newest complete frame, not yet claimed by the display
 *   live_held  - claimed by the display, being sent over SPI
 * The DMA is never pointed at live_held, so a frame on screen never tears.
 */

#define CAMERA_DMA_STREAM       DMA2_Stream1
#define CAMERA_DMA_FLAGS        (DMA_LIFCR_CFEIF1 | DMA_LIFCR_CDMEIF1 | \
                                 DMA_LIFCR_CTEIF1 | DMA_LIFCR_CHTIF1  | \
                                 DMA_LIFCR_CTCIF1)

static volatile bool     camera_live;
static volatile int32_t  live_cap;
static volatile int32_t  live_ready;
static volatile int32_t  live_held;
static volatile uint32_t live_frames;
static volatile uint32_t live_short_frames;
static volatile uint32_t live_dropped;

/** Point DMA2 Stream1 at a buffer and start it, bypassing HAL. */
static void Camera_LiveArm(int32_t index)
{
  DMA_Stream_TypeDef *s = CAMERA_DMA_STREAM;

  s->CR &= ~DMA_SxCR_EN;
  while ((s->CR & DMA_SxCR_EN) != 0U) { }

  DMA2->LIFCR = CAMERA_DMA_FLAGS;

  /* Keep the channel/burst/FIFO setup HAL_DMA_Init made in
   * Camera_MspDmaInit; only take the interrupts away from HAL, since the
   * DCMI end-of-frame drives everything here. */
  s->CR  &= ~(DMA_SxCR_TCIE | DMA_SxCR_HTIE | DMA_SxCR_TEIE |
              DMA_SxCR_DMEIE | DMA_SxCR_DBM | DMA_SxCR_CIRC);
  s->FCR &= ~DMA_SxFCR_FEIE;

  s->PAR  = (uint32_t)&DCMI->DR;
  s->M0AR = (uint32_t)camera_frame_buffers[index];
  s->NDTR = CAMERA_LIVE_WORD_COUNT;

  live_cap = index;

  s->CR |= DMA_SxCR_EN;
}

/** End of frame in live mode. Runs in the DCMI interrupt. */
static void Camera_LiveFrameEnd(void)
{
  DMA_Stream_TypeDef *s = CAMERA_DMA_STREAM;
  int32_t  done = live_cap;
  int32_t  next;
  uint32_t received;

  /* Disabling a peripheral-to-memory stream flushes its FIFO to memory
   * before EN reads back as 0, so the tail of the frame is in place. */
  s->CR &= ~DMA_SxCR_EN;
  while ((s->CR & DMA_SxCR_EN) != 0U) { }

  received = CAMERA_LIVE_WORD_COUNT - s->NDTR;

  if ((DCMI->RISR & DCMI_RIS_OVR_RIS) != 0U)
  {
    camera_overrun_count++;
    DCMI->ICR = DCMI_ICR_OVR_ISC;
  }

  if ((received + CAMERA_MAX_MISSING_WORDS) >= CAMERA_DMA_WORD_COUNT)
  {
    live_frames++;
    live_ready = done;
  }
  else
  {
    /* The first frame after arming, or one cut short by an overrun. */
    live_short_frames++;
  }

  /* Capture into the other buffer, unless the display is still sending it;
   * then the frame just finished is the only one free, and it is lost. */
  next = 1 - done;

  if (next == live_held)
  {
    next = done;
    live_dropped++;
  }

  if (live_ready == next)
  {
    live_ready = -1;
  }

  Camera_LiveArm(next);
}

Camera_Status_t Camera_StartLive(void)
{
  Camera_Stop();

  live_ready        = -1;
  live_held         = -1;
  live_frames       = 0U;
  live_short_frames = 0U;
  live_dropped      = 0U;

  /* Start in the blanking gap so the first frame is whole. */
  if (!Camera_WaitForFrameGap(1000U))
  {
    __HAL_DCMI_DISABLE(&hdcmi);
    return CAMERA_ERR_TIMEOUT;
  }

  camera_live = true;

  Camera_LiveArm(0);

  /* Continuous mode (CM = 0): the DCMI keeps capturing frame after frame.
   * Only the end-of-frame interrupt is used; overruns are read from RISR. */
  DCMI->ICR = DCMI_ICR_FRAME_ISC | DCMI_ICR_OVR_ISC | DCMI_ICR_ERR_ISC |
              DCMI_ICR_VSYNC_ISC | DCMI_ICR_LINE_ISC;
  DCMI->IER = DCMI_IER_FRAME_IE;
  DCMI->CR  = (DCMI->CR & ~DCMI_CR_CM) | DCMI_CR_ENABLE | DCMI_CR_CAPTURE;

  return CAMERA_OK;
}

void Camera_StopLive(void)
{
  if (!camera_live)
  {
    return;
  }

  DCMI->CR  &= ~DCMI_CR_CAPTURE;
  DCMI->IER  = 0U;

  CAMERA_DMA_STREAM->CR &= ~DMA_SxCR_EN;
  while ((CAMERA_DMA_STREAM->CR & DMA_SxCR_EN) != 0U) { }
  DMA2->LIFCR = CAMERA_DMA_FLAGS;

  __HAL_DCMI_DISABLE(&hdcmi);

  camera_live = false;
  live_ready  = -1;
  live_held   = -1;
}

int32_t Camera_LiveAcquire(void)
{
  int32_t index;

  __disable_irq();

  index = live_ready;

  if (index >= 0)
  {
    live_held  = index;
    live_ready = -1;
  }

  __enable_irq();

  return index;
}

void Camera_LiveRelease(void)
{
  live_held = -1;
}

uint32_t Camera_GetLiveFrameCount(void)
{
  return live_frames;
}

uint32_t Camera_GetLiveShortCount(void)
{
  return live_short_frames;
}

uint32_t Camera_GetLiveDroppedCount(void)
{
  return live_dropped;
}

void Camera_DcmiIrqHandler(void)
{
  uint32_t misr;

  if (!camera_live)
  {
    HAL_DCMI_IRQHandler(&hdcmi);
    return;
  }

  misr = DCMI->MISR;

  /* Clear everything except OVR, which Camera_LiveFrameEnd reads raw. */
  DCMI->ICR = DCMI_ICR_FRAME_ISC | DCMI_ICR_ERR_ISC |
              DCMI_ICR_VSYNC_ISC | DCMI_ICR_LINE_ISC;

  if ((misr & DCMI_MIS_FRAME_MIS) != 0U)
  {
    Camera_LiveFrameEnd();
  }
}

/* -------------------------------------------------------------------------- */
/* HAL callbacks                                                              */
/* -------------------------------------------------------------------------- */

void HAL_DCMI_FrameEventCallback(DCMI_HandleTypeDef *hdcmi_handle)
{
  if (hdcmi_handle->Instance == DCMI)
  {
    camera_frame_events++;
    camera_frame_ready = true;
  }
}

void HAL_DCMI_VsyncEventCallback(DCMI_HandleTypeDef *hdcmi_handle)
{
  if (hdcmi_handle->Instance == DCMI)
  {
    camera_vsync_events++;
  }
}

void Camera_GetDebugRegs(uint32_t *cr, uint32_t *sr, uint32_t *risr,
                         uint32_t *misr, uint32_t *frames, uint32_t *vsyncs)
{
  *cr     = camera_dbg_cr;
  *sr     = camera_dbg_sr;
  *risr   = camera_dbg_risr;
  *misr   = camera_dbg_misr;
  *frames = camera_frame_events;
  *vsyncs = camera_vsync_events;
}

void HAL_DCMI_ErrorCallback(DCMI_HandleTypeDef *hdcmi_handle)
{
  if (hdcmi_handle->Instance != DCMI)
  {
    return;
  }

  if ((hdcmi_handle->ErrorCode & HAL_DCMI_ERROR_OVR) != 0U)
  {
    camera_overrun_count++;
  }

  if ((hdcmi_handle->ErrorCode & HAL_DCMI_ERROR_SYNC) != 0U)
  {
    camera_sync_error_count++;
  }
}

/* -------------------------------------------------------------------------- */
/* UART frame dump                                                            */
/* -------------------------------------------------------------------------- */

/*
 * Wire format, little-endian throughout:
 *
 *   offset  size  field
 *   0       4     magic "OV56"
 *   4       2     width in pixels
 *   6       2     height in pixels
 *   8       1     bytes per pixel
 *   9       1     format code (0 = RGB565)
 *   10      4     payload length in bytes
 *   14      N     payload
 *   14+N    4     sum of all payload bytes, as a sanity check
 */

#define CAMERA_DUMP_FORMAT_RGB565   0U
#define CAMERA_DUMP_HEADER_SIZE     14U
#define CAMERA_DUMP_CHUNK_SIZE      4096U

Camera_Status_t Camera_DumpFrameUart(UART_HandleTypeDef *huart)
{
  uint8_t  header[CAMERA_DUMP_HEADER_SIZE];
  uint8_t  trailer[4];
  uint32_t length   = OV5640_FRAME_SIZE_BYTES;
  uint32_t checksum = 0U;

  header[0]  = 'O';
  header[1]  = 'V';
  header[2]  = '5';
  header[3]  = '6';
  header[4]  = (uint8_t)(OV5640_FRAME_WIDTH  & 0xFFU);
  header[5]  = (uint8_t)(OV5640_FRAME_WIDTH  >> 8);
  header[6]  = (uint8_t)(OV5640_FRAME_HEIGHT & 0xFFU);
  header[7]  = (uint8_t)(OV5640_FRAME_HEIGHT >> 8);
  header[8]  = (uint8_t)OV5640_BYTES_PER_PIXEL;
  header[9]  = (uint8_t)CAMERA_DUMP_FORMAT_RGB565;
  header[10] = (uint8_t)(length         & 0xFFU);
  header[11] = (uint8_t)((length >>  8) & 0xFFU);
  header[12] = (uint8_t)((length >> 16) & 0xFFU);
  header[13] = (uint8_t)((length >> 24) & 0xFFU);

  for (uint32_t i = 0U; i < length; i++)
  {
    checksum += camera_frame_buffer[i];
  }

  trailer[0] = (uint8_t)(checksum         & 0xFFU);
  trailer[1] = (uint8_t)((checksum >>  8) & 0xFFU);
  trailer[2] = (uint8_t)((checksum >> 16) & 0xFFU);
  trailer[3] = (uint8_t)((checksum >> 24) & 0xFFU);

  if (HAL_UART_Transmit(huart, header, sizeof(header), 1000U) != HAL_OK)
  {
    return CAMERA_ERR_TIMEOUT;
  }

  /* HAL_UART_Transmit takes a uint16_t size, and a QVGA RGB565 frame is
   * 153,600 bytes, so the payload has to go out in chunks. */
  for (uint32_t sent = 0U; sent < length; sent += CAMERA_DUMP_CHUNK_SIZE)
  {
    uint32_t chunk = length - sent;

    if (chunk > CAMERA_DUMP_CHUNK_SIZE)
    {
      chunk = CAMERA_DUMP_CHUNK_SIZE;
    }

    if (HAL_UART_Transmit(huart, &camera_frame_buffer[sent],
                          (uint16_t)chunk, 1000U) != HAL_OK)
    {
      return CAMERA_ERR_TIMEOUT;
    }
  }

  if (HAL_UART_Transmit(huart, trailer, sizeof(trailer), 1000U) != HAL_OK)
  {
    return CAMERA_ERR_TIMEOUT;
  }

  return CAMERA_OK;
}
