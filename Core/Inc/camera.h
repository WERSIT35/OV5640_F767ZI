/**
  ******************************************************************************
  * @file    camera.h
  * @brief   DCMI + DMA2 capture path for the OV5640.
  *
  * CubeMX did not generate a DMA request for DCMI, so the DMA stream, its
  * NVIC entry and the DCMI NVIC entry are all set up here.  Everything hooks
  * in through USER CODE blocks so a CubeMX regeneration will not wipe it.
  ******************************************************************************
  */

#ifndef __CAMERA_H
#define __CAMERA_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ov5640_conf.h"
#include <stdint.h>
#include <stdbool.h>

/**
  * @brief  Camera status.
  *
  * The numeric values are deliberately meaningful: each one doubles as the
  * LED blink count used by the bring-up diagnostics in main.c, so a failure
  * is readable off the board with no terminal attached.  Codes start at 2 so
  * that a single stray flash is never mistaken for a real code.
  */
typedef enum
{
  CAMERA_OK          = 0,
  CAMERA_ERR_I2C     = 2, /*!< SCCB transaction failed outright (no ACK)    */
  CAMERA_ERR_CHIP_ID = 3, /*!< SCCB works, but ID is not 0x5640             */
  CAMERA_ERR_CONFIG  = 4, /*!< ID good, a register table write failed       */
  CAMERA_ERR_DMA     = 5, /*!< HAL_DCMI_Start_DMA rejected the transfer     */
  CAMERA_ERR_TIMEOUT = 6, /*!< no end-of-frame within the timeout           */
  CAMERA_ERR_OVERRUN = 7, /*!< DCMI FIFO overrun - PIXCLK too fast for HCLK */
  CAMERA_ERR_SYNC    = 8, /*!< synchronisation error from the DCMI          */
  CAMERA_ERR_PARTIAL = 9, /*!< end-of-frame arrived, but only part of a frame*/
} Camera_Status_t;

/**
  * @brief  Start driving the sensor master clock on MCO1 (PA8).
  * @note   Only does anything if OV5640_DRIVE_XCLK_FROM_MCO1 is set.
  *         Camera_Init() calls this itself; it is exposed separately so
  *         bring-up diagnostics can get the clock running first. Safe to
  *         call more than once.
  */
void Camera_EnableXclk(void);

/**
  * @brief  Bring up the sensor and the DCMI/DMA path.
  * @note   Call after MX_DCMI_Init(), MX_I2C2_Init() and MX_GPIO_Init().
  */
Camera_Status_t Camera_Init(void);

/**
  * @brief  Capture exactly one frame into the internal buffer and block until
  *         the DCMI reports end-of-frame.
  * @param  timeout_ms  give up after this long (a frame takes ~1/fps).
  * @retval CAMERA_OK, or the reason the frame did not arrive.
  */
Camera_Status_t Camera_Capture(uint32_t timeout_ms);

/** Start free-running capture; Camera_IsFrameReady() polls for completion. */
Camera_Status_t Camera_StartContinuous(void);

/** Stop a continuous capture and disable the DCMI. */
void Camera_Stop(void);

/** True once a frame has landed. Clears the flag as a side effect. */
bool Camera_IsFrameReady(void);

/** Pointer to the frame buffer. Valid for OV5640_FRAME_SIZE_BYTES bytes. */
const uint8_t *Camera_GetBuffer(void);

/** Size of the frame buffer, in bytes. */
uint32_t Camera_GetBufferSize(void);

/** Number of DCMI overruns seen since reset; non-zero means PCLK is too fast. */
uint32_t Camera_GetOverrunCount(void);

/**
  * @brief  DMA words still outstanding when the last capture timed out.
  * @note   Compare against Camera_GetExpectedWords(): a small remainder means
  *         the frame nearly finished and only a few pixel clocks went
  *         missing; a remainder equal to the total means no data arrived.
  */
uint32_t Camera_GetTimeoutRemainingWords(void);

/** Total DMA words in one frame, for comparison against the above. */
uint32_t Camera_GetExpectedWords(void);

/** Arming attempts the last Camera_Capture() needed. 1 means first try. */
uint32_t Camera_GetLastAttempts(void);

/** Duration of the last successful capture, in milliseconds. */
uint32_t Camera_GetLastFrameMs(void);

/**
  * @brief  Whether the frame just returned lost data to a FIFO overrun.
  * @note   An overrun is not treated as a capture failure: it usually costs
  *         a handful of pixels at the end of a frame, and discarding an
  *         otherwise complete image over that is the worse trade. The frame
  *         may show a horizontal tear from the point data was lost.
  */
bool Camera_LastFrameHadOverrun(void);

/**
  * @brief  Stream the current frame out of a UART using the simple framed
  *         protocol understood by Tools/recv_frame.py.
  */
Camera_Status_t Camera_DumpFrameUart(UART_HandleTypeDef *huart);

/**
  * @brief  Initialise DMA2 Stream1 and link it to the DCMI handle.
  * @note   Called from HAL_DCMI_MspInit(); not intended for application use.
  */
void Camera_MspDmaInit(DCMI_HandleTypeDef *hdcmi);

/** Tear down the DMA stream. Called from HAL_DCMI_MspDeInit(). */
void Camera_MspDmaDeInit(DCMI_HandleTypeDef *hdcmi);

/** DMA2 Stream1 interrupt entry point, called from stm32f7xx_it.c. */
void Camera_DmaIrqHandler(void);

/**
  * @brief  DCMI register snapshot taken when the last capture timed out,
  *         plus running counts of frame-end and VSYNC interrupts.
  */
void Camera_GetDebugRegs(uint32_t *cr, uint32_t *sr, uint32_t *risr,
                         uint32_t *misr, uint32_t *frames, uint32_t *vsyncs);

#ifdef __cplusplus
}
#endif

#endif /* __CAMERA_H */
