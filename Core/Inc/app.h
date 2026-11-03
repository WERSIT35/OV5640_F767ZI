/**
  ******************************************************************************
  * @file    app.h
  * @brief   Camera application on top of the live preview: touch menu,
  *          status bar, picture controls, shooting aids, freeze/review.
  *
  * Called from the main loop only while the LCD is idle, because the touch
  * controller shares SPI1 with it:
  *
  *     App_Poll();                        // touch, buttons, timers
  *     if (App_WantsFrame()) {
  *         frame = <claimed camera buffer>;
  *         App_ProcessFrame(frame);       // zoom, peaking, overlays
  *         Lcd_BlitAsync(frame);
  *     }
  ******************************************************************************
  */

#ifndef __APP_H
#define __APP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/** Reset settings to defaults and push them to the sensor. */
void App_Init(void);

/** Touch, shutter button, self-timer.  LCD must be idle. */
void App_Poll(void);

/** False while a frozen frame is being reviewed. */
bool App_WantsFrame(void);

/** Apply the image tools and draw the UI into a claimed frame. */
void App_ProcessFrame(uint16_t *frame);

/** Frame rates for the status bar, updated once a second. */
void App_SetFps(uint32_t camera_fps, uint32_t display_fps);

/** True once if the user asked for touch recalibration from the menu. */
bool App_TakeCalibrationRequest(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_H */
