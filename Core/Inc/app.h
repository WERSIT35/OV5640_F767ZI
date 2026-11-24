/**
  ******************************************************************************
  * @file    app.h
  * @brief   Camera application: touch UI, picture controls, shooting aids and
  *          freeze/review on top of the live preview.
  ******************************************************************************
  */

#ifndef __APP_H
#define __APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "icons.h"

/** Load the saved settings and start the battery gauge. */
void App_Init(void);

/** Send every picture setting to the sensor (after a sensor restart too). */
void App_ApplySettings(void);

/** Poll buttons, touch, battery and card. Call while the LCD bus is free. */
void App_Poll(void);

/** False while the screen should keep what it shows (flash or review). */
bool App_WantsFrame(void);

/** Draw the tools and interface onto a 320x240 RGB565 frame. */
void App_ProcessFrame(uint16_t *frame);

/** Full-screen fault: a large struck-out icon. Blocking. */
void App_ShowFault(Icon_t icon);

/** Latest display rate, for the optional counter. */
void App_SetFps(uint32_t display_fps);

/** True once after the user asked for a touch calibration. */
bool App_TakeCalibrationRequest(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_H */
