/**
  ******************************************************************************
  * @file    ov5640.h
  * @brief   OV5640 sensor driver (SCCB register access + configuration).
  *
  * This layer only talks to the sensor.  Getting pixels into memory is the
  * job of camera.h / camera.c.
  ******************************************************************************
  */

#ifndef __OV5640_H
#define __OV5640_H

#ifdef __cplusplus
extern "C" {
#endif

#include "ov5640_conf.h"
#include <stdbool.h>

/* ov5640_regs.h holds the (large, static const) register tables and is
 * deliberately included only by ov5640.c. */

typedef enum
{
  OV5640_OK = 0,
  OV5640_ERR_I2C,       /*!< SCCB transaction failed (NACK or timeout)      */
  OV5640_ERR_CHIP_ID,   /*!< SCCB works, but the ID is not 0x5640           */
  OV5640_ERR_CONFIG,    /*!< ID good, but a configuration write failed      */
} OV5640_Status_t;

/* -------------------------------------------------------------------------- */
/* Low-level SCCB access                                                      */
/* -------------------------------------------------------------------------- */

HAL_StatusTypeDef OV5640_WriteReg(uint16_t reg, uint8_t val);
HAL_StatusTypeDef OV5640_ReadReg(uint16_t reg, uint8_t *val);
HAL_StatusTypeDef OV5640_ModifyReg(uint16_t reg, uint8_t mask, uint8_t val);

/* -------------------------------------------------------------------------- */
/* Power and identification                                                   */
/* -------------------------------------------------------------------------- */

/** Drive PWDN/RESET through the documented power-up sequence. Blocks ~30 ms. */
void OV5640_PowerUp(void);

/** Assert PWDN and hold the sensor in reset. */
void OV5640_PowerDown(void);

/** Read registers 0x300A/0x300B. Returns 0x5640 on a healthy part. */
HAL_StatusTypeDef OV5640_ReadChipId(uint16_t *id);

/* -------------------------------------------------------------------------- */
/* Configuration                                                              */
/* -------------------------------------------------------------------------- */

/**
  * @brief  Full sensor bring-up: power sequence, ID check, common register
  *         table, QVGA RGB565 preset, then start streaming.
  * @note   The I2C peripheral must already be initialised.
  */
OV5640_Status_t OV5640_Init(void);

/**
  * @brief  The chip ID read by the most recent OV5640_Init().
  * @note   Zero if the SCCB transaction itself failed.  Worth inspecting in
  *         the debugger: 0x0000 usually means nothing is driving SDA,
  *         0xFFFF means SDA is stuck high with no slave responding.
  */
uint16_t OV5640_GetLastChipId(void);

/** Enter (true) or leave (false) software standby; the DVP port goes idle. */
HAL_StatusTypeDef OV5640_SetStreaming(bool enable);

/** Mirror horizontally / flip vertically. Takes effect on the next frame. */
HAL_StatusTypeDef OV5640_SetMirrorFlip(bool mirror, bool flip);

/**
  * @brief  Enable the sensor's internal colour-bar test pattern.
  * @note   Useful for isolating DCMI/DMA problems from optics or lens issues:
  *         if the bars arrive cleanly the capture path is sound.
  */
HAL_StatusTypeDef OV5640_SetTestPattern(bool enable);

/** Brightness in the range -4..+4 (0 = neutral). */
HAL_StatusTypeDef OV5640_SetBrightness(int8_t level);

/** Contrast in the range -4..+4 (0 = neutral). */
HAL_StatusTypeDef OV5640_SetContrast(int8_t level);

/* -------------------------------------------------------------------------- */
/* Picture controls (values from OmniVision's OV5640 application notes)       */
/* -------------------------------------------------------------------------- */

typedef enum
{
  OV5640_EFFECT_NORMAL = 0,
  OV5640_EFFECT_MONO,
  OV5640_EFFECT_SEPIA,
  OV5640_EFFECT_NEGATIVE,
  OV5640_EFFECT_RED,
  OV5640_EFFECT_GREEN,
  OV5640_EFFECT_BLUE,
  OV5640_EFFECT_COUNT
} OV5640_Effect_t;

typedef enum
{
  OV5640_WB_AUTO = 0,
  OV5640_WB_SUNNY,
  OV5640_WB_CLOUDY,
  OV5640_WB_OFFICE,      /* fluorescent */
  OV5640_WB_HOME,        /* incandescent */
  OV5640_WB_COUNT
} OV5640_WhiteBalance_t;

/** Exposure compensation steps: -3..+3 in thirds of a stop (-1.0..+1.0 EV). */
#define OV5640_EV_MIN   (-3)
#define OV5640_EV_MAX   (3)

/** Colour effect via the special digital effects (SDE) block. */
HAL_StatusTypeDef OV5640_SetEffect(OV5640_Effect_t effect);

/** Auto or one of the manual white balance presets. */
HAL_StatusTypeDef OV5640_SetWhiteBalance(OV5640_WhiteBalance_t wb);

/** Shift the auto-exposure target, OV5640_EV_MIN..OV5640_EV_MAX. */
HAL_StatusTypeDef OV5640_SetExposureComp(int8_t step);

/** Night mode: lets AEC stretch frame time (lower fps) in low light. */
HAL_StatusTypeDef OV5640_SetNightMode(bool enable);

typedef enum
{
  OV5640_STYLE_STANDARD = 0,
  OV5640_STYLE_VIVID,
  OV5640_STYLE_NATURAL,
  OV5640_STYLE_COUNT
} OV5640_Style_t;

/** Colour saturation and sharpening preset. */
HAL_StatusTypeDef OV5640_SetStyle(OV5640_Style_t style);

#define OV5640_ZOOM_COUNT  3U

/** Sensor-crop zoom: 0 = 1x, 1 = 2x, 2 = 4x. Real detail, not doubled pixels. */
HAL_StatusTypeDef OV5640_SetZoom(uint8_t level);

#ifdef __cplusplus
}
#endif

#endif /* __OV5640_H */
