/**
  ******************************************************************************
  * @file    touch.h
  * @brief   XPT2046 resistive touch controller on the TFT module, sharing
  *          SPI1 with the ILI9341.
  *
  * Wiring: T_CLK -> PB3, T_DIN -> PB5, T_DO -> PB4, T_CS -> PF13,
  *         T_IRQ -> PE11 (low while the panel is pressed).
  *
  * The bus is shared, so every call here must happen while the LCD is idle
  * (Lcd_IsBusy() false).  While T_CS is low the SPI clock drops to
  * APB2/64 = 1.7 MHz, inside the XPT2046's 2.5 MHz limit, and is restored
  * afterwards so the LCD keeps its full speed.
  ******************************************************************************
  */

#ifndef __TOUCH_H
#define __TOUCH_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct
{
  bool     pressed;   /* finger on the panel with enough pressure         */
  uint16_t raw_x;     /* 12-bit ADC readings, averaged                     */
  uint16_t raw_y;
  uint16_t raw_z;     /* pressure (Z1); larger = firmer                    */
  int16_t  x;         /* screen coordinates after calibration, clamped to  */
  int16_t  y;         /* 0..LCD_WIDTH-1 / 0..LCD_HEIGHT-1                  */
} Touch_Point_t;

/**
  * @brief  Load the calibration saved in flash.
  * @retval true if a valid calibration was found; false means
  *         Touch_Calibrate() must run before coordinates are meaningful.
  */
bool Touch_Init(void);

/** True while T_IRQ reports the panel pressed. No SPI traffic. */
bool Touch_IsPressed(void);

/**
  * @brief  Sample the panel. Takes ~150 us when pressed, ~0 when not.
  * @note   LCD must be idle.  Fills `pt` either way; pt->pressed says
  *         whether the coordinates are valid.
  */
void Touch_Read(Touch_Point_t *pt);

/**
  * @brief  Interactive 3-point calibration: draws a crosshair at three
  *         spots in turn, waits for each to be tapped, solves the
  *         raw-to-screen mapping and saves it to flash.
  * @note   Blocking; only for start-up, never from the live loop.  Handles
  *         any rotation or mirroring between panel and display.
  */
void Touch_Calibrate(void);

#ifdef __cplusplus
}
#endif

#endif /* __TOUCH_H */
