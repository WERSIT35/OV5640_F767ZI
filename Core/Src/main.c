/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "camera.h"
#include "lcd.h"
#include "touch.h"
#include "ov5640.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* Red blink code for "HSE never became ready" - see Error_Handler.  Picks up
 * where the Camera_Status_t codes (2..9) leave off. */
#define DIAG_CODE_NO_HSE      10U

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

DCMI_HandleTypeDef hdcmi;

I2C_HandleTypeDef hi2c2;

SPI_HandleTypeDef hspi1;

UART_HandleTypeDef huart3;

/* USER CODE BEGIN PV */

/*
 * Live-preview statistics, refreshed once per second.  Also printed on the
 * UART, but kept in RAM so they can be read over SWD (STM32CubeProgrammer
 * or a debugger watch) with no serial link at all.
 */
typedef struct
{
  uint32_t camera_fps;        /* complete frames from the DCMI, per second  */
  uint32_t display_fps;       /* frames sent to the LCD, per second         */
  uint32_t overruns;          /* DCMI FIFO overruns since reset             */
  uint32_t short_frames;      /* frames that ended early                    */
  uint32_t dropped;           /* frames lost because the LCD was still busy */
  uint32_t fill_fps_x10;      /* full-screen fill rate at boot, x10         */
  uint32_t spi_hz;            /* LCD SPI clock                              */
} Preview_Stats_t;

volatile Preview_Stats_t preview_stats;

/*
 * PCLK tuning hook.  Write a sensor PLL system divider (1..15) here over SWD
 * - e.g. STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -w32 <addr> 4 - and
 * the main loop applies it to register 0x3035 between frames, so frame rate
 * against overruns can be explored without reflashing.  0 = no request.
 * The value that wins goes into OV5640_PLL_SYSDIV in ov5640_conf.h.
 */
volatile uint32_t pclk_sysdiv_request;
volatile uint32_t pclk_sysdiv_active = OV5640_PLL_SYSDIV;

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
static void MX_GPIO_Init(void);
static void MX_DCMI_Init(void);
static void MX_I2C2_Init(void);
static void MX_USART3_UART_Init(void);
static void MX_SPI1_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* ==========================================================================
 * Bring-up diagnostics - remove once the UART is confirmed working.
 *
 * LD1 (PB0) and LD3 (PB14) are the only two Nucleo LEDs still free here;
 * LD2 is on PB7, which this project uses for DCMI_VSYNC.
 *
 * During start-up the markers latch, so a hang is readable long afterwards:
 *   both dark        -> not executing at all
 *   red x10, repeat  -> HSE never came up (no 8 MHz MCO from the ST-LINK)
 *   green on         -> clock/SysTick/GPIO fine, stuck in an MX_*_Init
 *   green + red on   -> all init done, but the USART never asserted TC
 *   green off, red on-> UART transmitted; stuck in Camera_Init
 *
 * Both markers are cleared once the capture loop starts, so a steady red
 * during normal running means something is wrong rather than nothing.
 *
 * After that the two LEDs have distinct jobs:
 *   green toggling   -> live preview running (heartbeat, once a second)
 *   red blink codes  -> failure, flash count = Camera_Status_t value
 *   both alternating -> Error_Handler (an unexpected HAL failure)
 * ========================================================================== */

/* The I2C bus check, address scan and control-pin sweep add roughly three
 * seconds to every boot. Set to 0 once the camera is up. */
#define CAMERA_DIAG_VERBOSE   1

static void Diag_LedInit(void)
{
  GPIO_InitTypeDef led = {0};

  __HAL_RCC_GPIOB_CLK_ENABLE();

  led.Pin   = GPIO_PIN_0 | GPIO_PIN_14;
  led.Mode  = GPIO_MODE_OUTPUT_PP;
  led.Pull  = GPIO_NOPULL;
  led.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &led);
}

static void Diag_Blink(uint32_t times, uint32_t ms)
{
  for (uint32_t i = 0U; i < times; i++)
  {
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);
    HAL_Delay(ms);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_RESET);
    HAL_Delay(ms);
  }
}

/**
  * @brief  Flash the red LED `code` times, then hold dark for a beat.
  * @note   Green is forced off first so the count is unambiguous.
  */
static void Diag_BlinkCode(uint32_t code)
{
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0,  GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET);
  HAL_Delay(700U);

  for (uint32_t i = 0U; i < code; i++)
  {
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET);
    HAL_Delay(250U);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET);
    HAL_Delay(250U);
  }

  HAL_Delay(700U);
}

/**
  * @brief  Busy-wait roughly `ms` milliseconds without SysTick.
  * @note   For fault paths only, where interrupts are off and HAL_Delay would
  *         hang.  Scaled by SystemCoreClock so the blink rate is the same at
  *         16 MHz (HSE failed, still on HSI) and at 216 MHz.  The ~8 cycles
  *         per iteration is approximate; it only has to be readable by eye.
  */
static void Diag_SpinMs(uint32_t ms)
{
  uint32_t n = (SystemCoreClock / 8000U) * ms;

  for (volatile uint32_t i = 0U; i < n; i++) { }
}

/** Repeat a blink code forever. Never returns. */
static void Diag_BlinkCodeForever(uint32_t code)
{
  for (;;)
  {
    Diag_BlinkCode(code);
  }
}

/**
  * @brief  Push a string straight into USART3's data register.
  * @note   No HAL, no newlib, no interrupts - if this produces nothing but
  *         the LEDs blink, the fault is in the UART peripheral, the board's
  *         VCP wiring, or the terminal, not in the application code.
  */
static bool Diag_Puts(const char *s)
{
  uint32_t guard;

  while (*s != '\0')
  {
    guard = 2000000U;
    while ((USART3->ISR & USART_ISR_TXE) == 0U)
    {
      if (--guard == 0U) { return false; }
    }

    USART3->TDR = (uint8_t)*s++;
  }

  guard = 2000000U;
  while ((USART3->ISR & USART_ISR_TC) == 0U)
  {
    if (--guard == 0U) { return false; }
  }

  return true;
}

/** Small blocking printf-alike onto USART3, for status messages only. */
static void Log(const char *fmt, ...)
{
  char    buf[128];
  va_list args;
  int     len;

  va_start(args, fmt);
  len = vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  if (len > 0)
  {
    if ((size_t)len > sizeof(buf) - 1U)
    {
      len = (int)sizeof(buf) - 1;
    }

    HAL_UART_Transmit(&huart3, (uint8_t *)buf, (uint16_t)len, 500U);
  }
}

#if CAMERA_DIAG_VERBOSE

/**
  * @brief  Report the idle levels of SDA/SCL, with and without the MCU's
  *         internal pull-ups.
  * @note   An I2C bus at rest must sit HIGH on both lines.  Reading LOW with
  *         no internal pull-up means either nothing is pulling the bus up
  *         (no resistors on the module) or something is holding it down
  *         (unpowered slave, short, stuck transaction).  Leaves the pins
  *         configured for the I2C peripheral with internal pull-ups on.
  */
static void Diag_I2cBusCheck(void)
{
  GPIO_InitTypeDef pin = {0};

  __HAL_RCC_GPIOF_CLK_ENABLE();

  pin.Pin   = GPIO_PIN_0 | GPIO_PIN_1;
  pin.Mode  = GPIO_MODE_INPUT;
  pin.Pull  = GPIO_NOPULL;
  pin.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOF, &pin);
  HAL_Delay(2U);

  Log("I2C idle, floating:  SDA=%d SCL=%d\r\n",
      (int)HAL_GPIO_ReadPin(GPIOF, GPIO_PIN_0),
      (int)HAL_GPIO_ReadPin(GPIOF, GPIO_PIN_1));

  pin.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOF, &pin);
  HAL_Delay(2U);

  Log("I2C idle, pulled up: SDA=%d SCL=%d\r\n",
      (int)HAL_GPIO_ReadPin(GPIOF, GPIO_PIN_0),
      (int)HAL_GPIO_ReadPin(GPIOF, GPIO_PIN_1));

  /* Hand the pins back to I2C2, keeping the internal pull-ups engaged as a
   * fallback in case the module has no resistors of its own. They are weak
   * (~40k) but usually enough at 100 kHz over short wiring. */
  pin.Mode      = GPIO_MODE_AF_OD;
  pin.Pull      = GPIO_PULLUP;
  pin.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
  pin.Alternate = GPIO_AF4_I2C2;
  HAL_GPIO_Init(GPIOF, &pin);

  /* Taking the pins away from the peripheral and giving them back can leave
   * the I2C state machine believing a transfer is in flight (BUSY latched),
   * which later surfaces as HAL_I2C_ERROR_TIMEOUT instead of a clean NACK.
   * Toggling PE resets the state machine. */
  __HAL_I2C_DISABLE(&hi2c2);
  HAL_Delay(2U);
  __HAL_I2C_ENABLE(&hi2c2);
}

/** Dump the I2C peripheral state so a stuck bus is visible directly. */
static void Diag_I2cRegs(const char *when)
{
  Log("I2C %s: CR1=%08lX ISR=%08lX (BUSY=%lu) err=%08lX\r\n",
      when,
      (unsigned long)I2C2->CR1,
      (unsigned long)I2C2->ISR,
      (unsigned long)((I2C2->ISR >> 15) & 1UL),
      (unsigned long)hi2c2.ErrorCode);
}

/**
  * @brief  Try the chip-ID read under all four PWDN/RESET pin combinations.
  * @note   Resolves the PE2/PD7 mapping empirically instead of trusting the
  *         assumption baked into ov5640_conf.h.  If the two are swapped, the
  *         normal power-up sequence leaves the sensor held in reset AND
  *         powered down at once, which looks exactly like a dead sensor.
  */
static void Diag_CameraPinSweep(void)
{
  Log("Control-pin sweep:\r\n");

  for (int e = 0; e <= 1; e++)
  {
    for (int d = 0; d <= 1; d++)
    {
      uint16_t          id = 0U;
      HAL_StatusTypeDef st;

      HAL_GPIO_WritePin(GPIOE, GPIO_PIN_2,
                        e ? GPIO_PIN_SET : GPIO_PIN_RESET);
      HAL_GPIO_WritePin(GPIOD, GPIO_PIN_7,
                        d ? GPIO_PIN_SET : GPIO_PIN_RESET);

      /* Generous settle time: the sensor needs a few ms of XCLK after
       * reset is released before its SCCB block will answer. */
      HAL_Delay(100U);

      hi2c2.ErrorCode = HAL_I2C_ERROR_NONE;
      st = OV5640_ReadChipId(&id);

      Log("  PE2=%d PD7=%d -> %s id=0x%04X err=%08lX\r\n",
          e, d, (st == HAL_OK) ? "ACK " : "NACK",
          (unsigned)id, (unsigned long)hi2c2.ErrorCode);
    }
  }

  hi2c2.ErrorCode = HAL_I2C_ERROR_NONE;
}

/** Sweep every 7-bit address and report which ones acknowledge. */
static void Diag_I2cScan(void)
{
  uint32_t found = 0U;

  Log("I2C scan:");

  for (uint8_t addr = 0x08U; addr <= 0x77U; addr++)
  {
    if (HAL_I2C_IsDeviceReady(&hi2c2, (uint16_t)(addr << 1), 2U, 20U) == HAL_OK)
    {
      Log(" 0x%02X", (unsigned)addr);
      found++;
    }
  }

  if (found == 0U)
  {
    Log(" nothing responded");
  }

  Log("\r\n");
}

#endif /* CAMERA_DIAG_VERBOSE */

/**
  * @brief  Turn on the I- and D-cache, keeping the frame buffers uncached.
  * @note   MPU region 1 covers 0x20000000-0x2007FFFF as normal, non-cacheable
  *         memory, with its first two 64K subregions disabled so the DTCM
  *         (0x20000000-0x2001FFFF) keeps the default map - the DTCM is never
  *         cached anyway.  What is left is SRAM1+SRAM2, which the linker
  *         script reserves for .framebuf.
  *
  *         Chosen over cache maintenance (invalidate after DCMI, clean before
  *         SPI DMA) because the CPU never reads or writes pixels on the live
  *         path: caching the buffers would buy nothing, and every missed or
  *         mis-sized maintenance call would show up as stale stripes.
  *         Everything else - code in flash, variables and stack in DTCM -
  *         gets the full benefit of the caches.
  *
  *         Region 0 is CubeMX's background region (MPU_Config); region 1 has
  *         the higher number, so it wins where they overlap.
  */
static void Cache_Config(void)
{
  MPU_Region_InitTypeDef region = {0};

  HAL_MPU_Disable();

  region.Enable           = MPU_REGION_ENABLE;
  region.Number           = MPU_REGION_NUMBER1;
  region.BaseAddress      = 0x20000000U;
  region.Size             = MPU_REGION_SIZE_512KB;
  region.SubRegionDisable = 0x03U;
  region.TypeExtField     = MPU_TEX_LEVEL1;
  region.AccessPermission = MPU_REGION_FULL_ACCESS;
  region.DisableExec      = MPU_INSTRUCTION_ACCESS_DISABLE;
  region.IsShareable      = MPU_ACCESS_NOT_SHAREABLE;
  region.IsCacheable      = MPU_ACCESS_NOT_CACHEABLE;
  region.IsBufferable     = MPU_ACCESS_NOT_BUFFERABLE;
  HAL_MPU_ConfigRegion(&region);

  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

  SCB_EnableICache();
  SCB_EnableDCache();
}

/**
  * @brief  Display bring-up: solid red, green, blue, then a gradient, then a
  *         timed run of full-screen fills to measure the raw SPI frame rate.
  * @note   Runs once at boot, before live preview, so blocking is fine here.
  */
static void Lcd_SelfTest(void)
{
  static const uint16_t solid[] = { LCD_RED, LCD_GREEN, LCD_BLUE };
  uint16_t *pixels = (uint16_t *)Camera_GetFrameBuffer(0U);
  uint32_t  start;
  uint32_t  elapsed;
  const uint32_t fills = 30U;

  for (uint32_t i = 0U; i < 3U; i++)
  {
    Lcd_FillAsync(solid[i]);
    Lcd_WaitIdle();
    HAL_Delay(400U);
  }

  /* Red rises left to right, green top to bottom, blue falls left to right:
   * every channel and both axes are visible at a glance. */
  for (uint32_t y = 0U; y < LCD_HEIGHT; y++)
  {
    for (uint32_t x = 0U; x < LCD_WIDTH; x++)
    {
      uint32_t r = (x * 31U) / (LCD_WIDTH - 1U);
      uint32_t g = (y * 63U) / (LCD_HEIGHT - 1U);
      uint32_t b = 31U - r;

      pixels[y * LCD_WIDTH + x] = (uint16_t)((r << 11) | (g << 5) | b);
    }
  }

  Lcd_BlitAsync(pixels);
  Lcd_WaitIdle();
  HAL_Delay(800U);

  start = HAL_GetTick();

  for (uint32_t i = 0U; i < fills; i++)
  {
    Lcd_FillAsync((i & 1U) ? LCD_WHITE : LCD_BLACK);
  }

  Lcd_WaitIdle();
  elapsed = HAL_GetTick() - start;

  preview_stats.spi_hz       = Lcd_GetSpiHz();
  preview_stats.fill_fps_x10 = (elapsed > 0U) ? (fills * 10000U) / elapsed : 0U;

  Log("LCD: SPI %lu Hz, full-screen fills %lu.%lu fps\r\n",
      (unsigned long)preview_stats.spi_hz,
      (unsigned long)(preview_stats.fill_fps_x10 / 10U),
      (unsigned long)(preview_stats.fill_fps_x10 % 10U));
}

/* ==========================================================================
 * Touch test overlay: a red dot under the finger and a yellow trail that
 * fades after TRAIL_MS, drawn into the camera frame just before it is sent
 * to the LCD.  The frame buffer is claimed by the display at that point, so
 * the camera cannot overwrite the overlay mid-blit.
 * ========================================================================== */

#define TRAIL_LEN        160U
#define TRAIL_MS         1500U
#define TOUCH_POLL_MS    10U

#define COLOUR_YELLOW    0xFFE0U

typedef struct
{
  int16_t  x;
  int16_t  y;
  uint32_t t;
} Trail_Point_t;

/* Latest reading, also handy to watch over SWD. */
typedef struct
{
  uint32_t presses;           /* press events since reset                   */
  uint16_t raw_x, raw_y, raw_z;
  int16_t  x, y;
  uint8_t  pressed;
} Touch_Stats_t;

volatile Touch_Stats_t touch_stats;

static Trail_Point_t trail[TRAIL_LEN];
static uint32_t      trail_head;
static Touch_Point_t touch_now;

/** Sample the panel at most every TOUCH_POLL_MS. LCD must be idle. */
static void TouchTest_Poll(void)
{
  static uint32_t last_poll;
  static bool     was_pressed;
  uint32_t        now = HAL_GetTick();

  if ((now - last_poll) < TOUCH_POLL_MS)
  {
    return;
  }

  last_poll = now;

  Touch_Read(&touch_now);

  if (touch_now.pressed)
  {
    const Trail_Point_t *prev = &trail[(trail_head + TRAIL_LEN - 1U) % TRAIL_LEN];
    int32_t dx = touch_now.x - prev->x;
    int32_t dy = touch_now.y - prev->y;

    /* Only record movement, so holding still does not flood the trail. */
    if (!was_pressed || ((dx * dx) + (dy * dy) >= 4))
    {
      trail[trail_head].x = touch_now.x;
      trail[trail_head].y = touch_now.y;
      trail[trail_head].t = now;
      trail_head = (trail_head + 1U) % TRAIL_LEN;
    }

    if (!was_pressed)
    {
      touch_stats.presses++;
    }

    touch_stats.raw_x = touch_now.raw_x;
    touch_stats.raw_y = touch_now.raw_y;
    touch_stats.raw_z = touch_now.raw_z;
    touch_stats.x     = touch_now.x;
    touch_stats.y     = touch_now.y;
  }

  was_pressed         = touch_now.pressed;
  touch_stats.pressed = touch_now.pressed ? 1U : 0U;
}

/** Fill a (2r+1)-square centred on (x, y), clipped to the frame. */
static void Frame_Dot(uint16_t *frame, int32_t x, int32_t y, int32_t r,
                      uint16_t colour)
{
  for (int32_t yy = y - r; yy <= y + r; yy++)
  {
    if ((yy < 0) || (yy >= (int32_t)LCD_HEIGHT)) { continue; }

    for (int32_t xx = x - r; xx <= x + r; xx++)
    {
      if ((xx < 0) || (xx >= (int32_t)LCD_WIDTH)) { continue; }

      frame[(yy * (int32_t)LCD_WIDTH) + xx] = colour;
    }
  }
}

static void TouchTest_Draw(uint16_t *frame)
{
  uint32_t now = HAL_GetTick();

  for (uint32_t i = 0U; i < TRAIL_LEN; i++)
  {
    if ((trail[i].t != 0U) && ((now - trail[i].t) < TRAIL_MS))
    {
      Frame_Dot(frame, trail[i].x, trail[i].y, 1, COLOUR_YELLOW);
    }
  }

  if (touch_now.pressed)
  {
    Frame_Dot(frame, touch_now.x, touch_now.y, 5, LCD_WHITE);
    Frame_Dot(frame, touch_now.x, touch_now.y, 4, LCD_RED);
  }
}

static const char *Camera_StatusName(Camera_Status_t status)
{
  switch (status)
  {
    case CAMERA_OK:            return "OK";
    case CAMERA_ERR_I2C:       return "I2C (no ACK from the sensor)";
    case CAMERA_ERR_CHIP_ID:   return "CHIP_ID (answered, but not 0x5640)";
    case CAMERA_ERR_CONFIG:    return "CONFIG (register write failed)";
    case CAMERA_ERR_DMA:       return "DMA";
    case CAMERA_ERR_TIMEOUT:   return "TIMEOUT (no end-of-frame)";
    case CAMERA_ERR_OVERRUN:   return "OVERRUN (PIXCLK too fast for HCLK)";
    case CAMERA_ERR_SYNC:      return "SYNC";
    case CAMERA_ERR_PARTIAL:   return "PARTIAL (armed mid-frame)";
    default:                   return "?";
  }
}

/**
  * @brief  UART debug mode: take one snapshot and dump it in the framed
  *         format Tools/recv_frame.py and Tools/live_view.py expect.
  * @note   Live preview must be stopped first; the snapshot lands in frame
  *         buffer 0 and is also shown on the LCD.  Frames go out only when
  *         the host asks with 'c', so the port stays readable text the rest
  *         of the time instead of a binary stream to dig the log out of.
  */
static void Debug_SnapshotToUart(void)
{
  Camera_Status_t status;

  Log("capture...\r\n");

  status = Camera_Capture(3000U);

  Log("  -> %s after %lu attempt(s), %lu/%lu words left\r\n",
      Camera_StatusName(status),
      (unsigned long)Camera_GetLastAttempts(),
      (unsigned long)Camera_GetTimeoutRemainingWords(),
      (unsigned long)Camera_GetExpectedWords());

  if (status == CAMERA_OK)
  {
    if (Camera_LastFrameHadOverrun())
    {
      Log("frame ok but overran (%lu total) - may show a tear; "
          "raise OV5640_PLL_SYSDIV\r\n",
          (unsigned long)Camera_GetOverrunCount());
    }

    Lcd_BlitAsync((const uint16_t *)Camera_GetBuffer());
    Camera_DumpFrameUart(&huart3);
    Lcd_WaitIdle();
  }
  else
  {
    uint32_t cr, sr, risr, misr, frames, vsyncs;

    Log("capture failed: %s (overruns %lu, DMA %lu/%lu words left)\r\n",
        Camera_StatusName(status),
        (unsigned long)Camera_GetOverrunCount(),
        (unsigned long)Camera_GetTimeoutRemainingWords(),
        (unsigned long)Camera_GetExpectedWords());

    Camera_GetDebugRegs(&cr, &sr, &risr, &misr, &frames, &vsyncs);
    Log("  DCMI CR=%08lX SR=%08lX RISR=%08lX MISR=%08lX\r\n",
        (unsigned long)cr, (unsigned long)sr,
        (unsigned long)risr, (unsigned long)misr);
    Log("  IRQ counts: frame=%lu vsync=%lu, attempts=%lu\r\n",
        (unsigned long)frames, (unsigned long)vsyncs,
        (unsigned long)Camera_GetLastAttempts());

    /* One red pulse per failure. The full blink code is reserved for
     * Camera_Init, where there is no working UART to fall back on - here
     * the log line above says far more, far faster. */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET);
    HAL_Delay(60U);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET);
  }
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MPU Configuration--------------------------------------------------------*/
  MPU_Config();

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */
  Cache_Config();
  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  /* Marker 1: clock, SysTick and GPIO are alive. Green latches on. */
  Diag_LedInit();
  Diag_Blink(3U, 150U);
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0, GPIO_PIN_SET);
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DCMI_Init();
  MX_I2C2_Init();
  MX_USART3_UART_Init();
  MX_SPI1_Init();
  /* USER CODE BEGIN 2 */
  /* Marker 2: every MX_*_Init returned. Red latches on. */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET);

  /* Raw first, so a failure here rules newlib out as the cause. */
  {
    bool uart_ok = Diag_Puts("\r\nRAW UART OK\r\n");

    /* Marker 3: green goes OUT once the UART has actually shifted those
     * bytes out (TC set).  Green staying ON means TXE/TC never asserted,
     * i.e. the USART itself is dead rather than the link to the PC. */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0,
                      uart_ok ? GPIO_PIN_RESET : GPIO_PIN_SET);
  }

  Log("\r\n=== OV5640 / DCMI capture ===\r\n");
  Log("SYSCLK %lu Hz, PCLK1 %lu Hz, PCLK2 %lu Hz, USART3 %lu baud\r\n",
      (unsigned long)HAL_RCC_GetSysClockFreq(),
      (unsigned long)HAL_RCC_GetPCLK1Freq(),
      (unsigned long)HAL_RCC_GetPCLK2Freq(),
      (unsigned long)huart3.Init.BaudRate);
  Log("HCLK %lu Hz -> max DCMI PIXCLK ~%lu Hz\r\n",
      (unsigned long)HAL_RCC_GetHCLKFreq(),
      (unsigned long)((HAL_RCC_GetHCLKFreq() * 2UL) / 5UL));

#if CAMERA_DIAG_VERBOSE
  Diag_I2cRegs("before   ");
  Diag_I2cBusCheck();
  Diag_I2cRegs("after PE ");

  /* Clear accumulated errors so each stage's code is attributable - HAL ORs
   * into ErrorCode and never clears it by itself. */
  hi2c2.ErrorCode = HAL_I2C_ERROR_NONE;
  Diag_I2cScan();
  Diag_I2cRegs("after scan");
  hi2c2.ErrorCode = HAL_I2C_ERROR_NONE;

  /* Get XCLK running before probing: without it the sensor cannot answer
   * SCCB at all, and the pin sweep below would be meaningless. */
  Camera_EnableXclk();
  Diag_CameraPinSweep();
#endif

  {
    Camera_Status_t status = Camera_Init();

    if (status != CAMERA_OK)
    {
      Log("Camera_Init failed: %s (chip ID read back as 0x%04X)\r\n",
          Camera_StatusName(status), (unsigned)OV5640_GetLastChipId());
      Log("I2C ErrorCode 0x%08lX\r\n", (unsigned long)hi2c2.ErrorCode);

      /* Blink the failure out rather than halting silently: the enum value
       * is the flash count. See Camera_Status_t in camera.h. */
      Diag_BlinkCodeForever((uint32_t)status);
    }
  }

  Log("OV5640 ready: %ux%u RGB565, %lu bytes/frame\r\n",
      (unsigned)OV5640_FRAME_WIDTH, (unsigned)OV5640_FRAME_HEIGHT,
      (unsigned long)Camera_GetBufferSize());
  Log("send 'c' to receive one frame\r\n");

  Lcd_Init();
  Lcd_SelfTest();

  /* Calibrate on first boot (nothing saved yet), or when the shutter button
   * (PF14) is held while the board starts. */
  if (!Touch_Init() ||
      (HAL_GPIO_ReadPin(SHUTTER_GPIO_Port, SHUTTER_Pin) == GPIO_PIN_RESET))
  {
    Log("touch: calibrating - tap the three crosshairs\r\n");
    Touch_Calibrate();
  }

  {
    Camera_Status_t status = Camera_StartLive();

    if (status != CAMERA_OK)
    {
      Log("Camera_StartLive failed: %s\r\n", Camera_StatusName(status));
      Diag_BlinkCodeForever((uint32_t)status);
    }
  }

  Log("live preview running\r\n");

  /* Bring-up is over, so drop the init marker.  From here the LEDs report
   * running state: green toggles once a second, red means trouble. */
  HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    /*
     * Live preview.  Nothing here blocks: each pass checks the UART for a
     * debug request, hands the newest camera frame to the LCD if the LCD is
     * free, and once a second publishes frame-rate statistics.
     */
    static bool     lcd_holds_frame = false;
    static uint32_t stats_start     = 0U;
    static uint32_t camera_base     = 0U;
    static uint32_t display_base    = 0U;

    /* Debug path: 'c' on the UART stops the preview, takes a snapshot,
     * dumps it for Tools/live_view.py / recv_frame.py, and resumes. */
    if ((USART3->ISR & USART_ISR_ORE) != 0U)
    {
      USART3->ICR = USART_ICR_ORECF;
    }

    if ((USART3->ISR & USART_ISR_RXNE) != 0U)
    {
      uint8_t cmd = (uint8_t)USART3->RDR;

      if ((cmd == 'c') || (cmd == 'C'))
      {
        Lcd_WaitIdle();
        Camera_LiveRelease();
        lcd_holds_frame = false;

        Camera_StopLive();
        Debug_SnapshotToUart();

        if (Camera_StartLive() != CAMERA_OK)
        {
          Log("live preview failed to restart\r\n");
        }

        camera_base = Camera_GetLiveFrameCount();
      }
    }

    if ((pclk_sysdiv_request >= 1U) && (pclk_sysdiv_request <= 15U) &&
        (pclk_sysdiv_request != pclk_sysdiv_active))
    {
      uint32_t sysdiv = pclk_sysdiv_request;

      if (OV5640_WriteReg(0x3035U,   /* SC_PLL_CTRL1: [7:4] sysdiv */
                          (uint8_t)((sysdiv << 4) | 0x01U)) == HAL_OK)
      {
        pclk_sysdiv_active = sysdiv;
        Log("sensor sysdiv -> %lu\r\n", (unsigned long)sysdiv);
      }

      pclk_sysdiv_request = 0U;
    }

    if (!Lcd_IsBusy())
    {
      int32_t index;

      if (lcd_holds_frame)
      {
        Camera_LiveRelease();
        lcd_holds_frame = false;
      }

      /* The bus is free between blits, so this is the only safe moment to
       * talk to the touch controller. */
      TouchTest_Poll();

      index = Camera_LiveAcquire();

      if (index >= 0)
      {
        uint16_t *frame = (uint16_t *)Camera_GetFrameBuffer((uint32_t)index);

        lcd_holds_frame = true;
        TouchTest_Draw(frame);
        Lcd_BlitAsync(frame);
      }
    }

    if ((HAL_GetTick() - stats_start) >= 1000U)
    {
      uint32_t now_ms   = HAL_GetTick();
      uint32_t camera   = Camera_GetLiveFrameCount();
      uint32_t display  = Lcd_GetFrameCount();
      uint32_t interval = now_ms - stats_start;

      preview_stats.camera_fps   = ((camera - camera_base) * 1000U) / interval;
      preview_stats.display_fps  = ((display - display_base) * 1000U) / interval;
      preview_stats.overruns     = Camera_GetOverrunCount();
      preview_stats.short_frames = Camera_GetLiveShortCount();
      preview_stats.dropped      = Camera_GetLiveDroppedCount();

      Log("camera %lu fps, display %lu fps, overruns %lu, short %lu, "
          "dropped %lu\r\n",
          (unsigned long)preview_stats.camera_fps,
          (unsigned long)preview_stats.display_fps,
          (unsigned long)preview_stats.overruns,
          (unsigned long)preview_stats.short_frames,
          (unsigned long)preview_stats.dropped);

      camera_base  = camera;
      display_base = display;
      stats_start  = now_ms;

      /* Heartbeat: green toggles once a second while preview runs. */
      HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_0);
    }
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 8;
  RCC_OscInitStruct.PLL.PLLN = 216;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 9;
  RCC_OscInitStruct.PLL.PLLR = 2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Activate the Over-Drive mode
  */
  if (HAL_PWREx_EnableOverDrive() != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_7) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief DCMI Initialization Function
  * @param None
  * @retval None
  */
static void MX_DCMI_Init(void)
{

  /* USER CODE BEGIN DCMI_Init 0 */

  /* USER CODE END DCMI_Init 0 */

  /* USER CODE BEGIN DCMI_Init 1 */

  /* USER CODE END DCMI_Init 1 */
  hdcmi.Instance = DCMI;
  hdcmi.Init.SynchroMode = DCMI_SYNCHRO_HARDWARE;
  hdcmi.Init.PCKPolarity = DCMI_PCKPOLARITY_RISING;
  hdcmi.Init.VSPolarity = DCMI_VSPOLARITY_HIGH;
  hdcmi.Init.HSPolarity = DCMI_HSPOLARITY_LOW;
  hdcmi.Init.CaptureRate = DCMI_CR_ALL_FRAME;
  hdcmi.Init.ExtendedDataMode = DCMI_EXTEND_DATA_8B;
  hdcmi.Init.JPEGMode = DCMI_JPEG_DISABLE;
  hdcmi.Init.ByteSelectMode = DCMI_BSM_ALL;
  hdcmi.Init.ByteSelectStart = DCMI_OEBS_ODD;
  hdcmi.Init.LineSelectMode = DCMI_LSM_ALL;
  hdcmi.Init.LineSelectStart = DCMI_OELS_ODD;
  if (HAL_DCMI_Init(&hdcmi) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN DCMI_Init 2 */
  /*
   * Polarities above match the OV5640's default DVP timing, not CubeMX's
   * defaults: the sensor changes data on the falling PCLK edge (so the DCMI
   * must sample on the rising edge) and pulses VSYNC high during vertical
   * blanking (so the "data invalid" level is HIGH).  These are set in the
   * .ioc as well, but check the DCMI tab after any pin change.
   *
   * HAL_DCMI_Init() enables the LINE and VSYNC interrupts, which would fire
   * 240 times per frame for no benefit here.  Keep only the error sources.
   */
  __HAL_DCMI_DISABLE_IT(&hdcmi, DCMI_IT_LINE | DCMI_IT_VSYNC);
  /* USER CODE END DCMI_Init 2 */

}

/**
  * @brief I2C2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C2_Init(void)
{

  /* USER CODE BEGIN I2C2_Init 0 */

  /* USER CODE END I2C2_Init 0 */

  /* USER CODE BEGIN I2C2_Init 1 */

  /* USER CODE END I2C2_Init 1 */
  hi2c2.Instance = I2C2;
  hi2c2.Init.Timing = 0x20404768;
  hi2c2.Init.OwnAddress1 = 0;
  hi2c2.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c2.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c2.Init.OwnAddress2 = 0;
  hi2c2.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c2.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c2.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c2) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Analogue filter
  */
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c2, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Digital filter
  */
  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c2, 0) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C2_Init 2 */

  /* USER CODE END I2C2_Init 2 */

}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES;
  hspi1.Init.DataSize = SPI_DATASIZE_8BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_LOW;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 7;
  hspi1.Init.CRCLength = SPI_CRC_LENGTH_DATASIZE;
  hspi1.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

}

/**
  * @brief USART3 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART3_UART_Init(void)
{

  /* USER CODE BEGIN USART3_Init 0 */

  /* USER CODE END USART3_Init 0 */

  /* USER CODE BEGIN USART3_Init 1 */

  /* USER CODE END USART3_Init 1 */
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 2000000;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_16;
  huart3.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart3.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart3) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART3_Init 2 */

  /* USER CODE END USART3_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOE_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOE, GPIO_PIN_2, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOF, LCD_RST_Pin|T_CS_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LCD_CS_GPIO_Port, LCD_CS_Pin, GPIO_PIN_SET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOD, LCD_DC_Pin|GPIO_PIN_7, GPIO_PIN_RESET);

  /*Configure GPIO pin : PE2 */
  GPIO_InitStruct.Pin = GPIO_PIN_2;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOE, &GPIO_InitStruct);

  /*Configure GPIO pin : LCD_RST_Pin */
  GPIO_InitStruct.Pin = LCD_RST_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(LCD_RST_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : T_CS_Pin */
  GPIO_InitStruct.Pin = T_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(T_CS_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : SHUTTER_Pin */
  GPIO_InitStruct.Pin = SHUTTER_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(SHUTTER_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : T_IRQ_Pin */
  GPIO_InitStruct.Pin = T_IRQ_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(T_IRQ_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : LCD_CS_Pin LCD_DC_Pin */
  GPIO_InitStruct.Pin = LCD_CS_Pin|LCD_DC_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
  HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

  /*Configure GPIO pin : PD7 */
  GPIO_InitStruct.Pin = GPIO_PIN_7;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOD, &GPIO_InitStruct);

  /* EXTI interrupt init*/
  HAL_NVIC_SetPriority(EXTI15_10_IRQn, 10, 0);
  HAL_NVIC_EnableIRQ(EXTI15_10_IRQn);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

 /* MPU Configuration */

void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};

  /* Disables the MPU */
  HAL_MPU_Disable();

  /** Initializes and configures the Region and the memory to be protected
  */
  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x0;
  MPU_InitStruct.Size = MPU_REGION_SIZE_4GB;
  MPU_InitStruct.SubRegionDisable = 0x87;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL0;
  MPU_InitStruct.AccessPermission = MPU_REGION_NO_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_DISABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  /* Enables the MPU */
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);

}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  __disable_irq();

  /* SystemClock_Config runs before Diag_LedInit, so a clock failure lands
   * here with the LED pins still unconfigured.  Initialising them again is
   * harmless if they already are. */
  Diag_LedInit();

  /* HSE bypass relies on the ST-LINK's 8 MHz MCO.  If it was requested but
   * never arrived, say so specifically rather than with the generic pattern
   * below.  The PLL currently runs from HSI (this board's MCO does not reach
   * PH0), so HSEON is clear and this only fires if HSE is switched back on. */
  if (((RCC->CR & RCC_CR_HSEON) != 0U) && ((RCC->CR & RCC_CR_HSERDY) == 0U))
  {
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_0 | GPIO_PIN_14, GPIO_PIN_RESET);

    for (;;)
    {
      for (uint32_t i = 0U; i < DIAG_CODE_NO_HSE; i++)
      {
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_SET);
        Diag_SpinMs(250U);
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET);
        Diag_SpinMs(250U);
      }

      Diag_SpinMs(1400U);
    }
  }

  while (1)
  {
    /* Flash both LEDs together so reaching Error_Handler is distinguishable
     * from a hang.  Busy-wait rather than HAL_Delay: interrupts are off, so
     * SysTick never advances and HAL_Delay would block forever. */
    HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_0 | GPIO_PIN_14);

    Diag_SpinMs(200U);
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
