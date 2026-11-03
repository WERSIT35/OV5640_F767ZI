/**
  ******************************************************************************
  * @file    lcd.h
  * @brief   ILI9341 320x240 TFT on SPI1, with DMA2 Stream3 full-frame blits.
  *
  * Wiring (TPM408-2.8 module):
  *   SCK PB3, MOSI PB5, CS PD14, DC PD15, RST PF12, backlight tied to 3V3.
  *   The panel's SDO is left unconnected - the driver never reads it, and on
  *   these modules it does not tri-state, which would upset the XPT2046
  *   sharing MISO (PB4).
  *
  * Pixels are sent as 16-bit SPI frames, MSB first, straight out of memory.
  * A little-endian uint16 RGB565 buffer - which is what the DCMI produces
  * with OV5640 register 0x4300 = 0x6F - therefore needs no byte swapping.
  ******************************************************************************
  */

#ifndef __LCD_H
#define __LCD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"
#include <stdint.h>
#include <stdbool.h>

#define LCD_WIDTH    320U
#define LCD_HEIGHT   240U

/*
 * SPI1 clock divider from APB2 (108 MHz): 2, 4, 8, 16, 32, 64, 128 or 256.
 *
 *   2  -> 54   MHz  fastest; past the ILI9341 datasheet, needs short wiring
 *   4  -> 27   MHz  the usual safe choice for jumper wires
 *   16 -> 6.75 MHz  bring-up: rules signal integrity out entirely
 *
 * A white screen means the panel never saw a valid init sequence; noise,
 * wrong colours in patches or a frozen picture mean the clock is too fast
 * for the wiring.  Either way, raise this.
 */
#define LCD_SPI_DIV    2U

/*
 * Memory access control (0x36).  0x28 = landscape (MV) + BGR panel order.
 * If the image is upside down use 0xE8; if it is mirrored use 0x68 or 0xA8.
 */
#define LCD_MADCTL     0x28U

/* RGB565 colours. */
#define LCD_BLACK      0x0000U
#define LCD_WHITE      0xFFFFU
#define LCD_RED        0xF800U
#define LCD_GREEN      0x07E0U
#define LCD_BLUE       0x001FU

/** Reset and initialise the panel. Blocking (~300 ms of datasheet delays). */
void Lcd_Init(void);

/** Fill the whole screen with one colour via DMA. Returns immediately. */
void Lcd_FillAsync(uint16_t colour);

/**
  * @brief  Send a full 320x240 RGB565 frame via DMA. Returns immediately.
  * @param  pixels  76800 little-endian RGB565 pixels.  Must stay untouched
  *                 until Lcd_IsBusy() goes false.  Must not be cached (the
  *                 frame buffers live in the non-cacheable .framebuf region).
  */
void Lcd_BlitAsync(const uint16_t *pixels);

/** True while a fill or blit is still being clocked out. */
bool Lcd_IsBusy(void);

/** Block until the current transfer has finished. */
void Lcd_WaitIdle(void);

/** Fill a rectangle with one colour (blocking, polled - for small shapes). */
void Lcd_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                  uint16_t colour);

/** Number of fills/blits completed since reset. */
uint32_t Lcd_GetFrameCount(void);

/** SPI clock actually in use, in Hz. */
uint32_t Lcd_GetSpiHz(void);

/** DMA2 Stream3 interrupt entry point, called from stm32f7xx_it.c. */
void Lcd_DmaIrqHandler(void);

#ifdef __cplusplus
}
#endif

#endif /* __LCD_H */
