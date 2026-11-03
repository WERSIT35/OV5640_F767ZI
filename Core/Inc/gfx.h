/**
  ******************************************************************************
  * @file    gfx.h
  * @brief   Drawing into a 320x240 RGB565 frame buffer before it is sent to
  *          the LCD: text, boxes, and the per-frame image tools (zoom, focus
  *          peaking, zebra, histogram).
  *
  * Everything here works on the camera frame the display has claimed, so it
  * is the CPU's only per-pixel work on the live path.  The frame buffers are
  * non-cacheable, so whole-frame passes cost a few milliseconds each.
  ******************************************************************************
  */

#ifndef __GFX_H
#define __GFX_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

#define GFX_W   320
#define GFX_H   240

/** Pack 8-bit RGB into RGB565. */
#define GFX_RGB(r, g, b) \
  ((uint16_t)((((r) & 0xF8U) << 8) | (((g) & 0xFCU) << 3) | ((b) >> 3)))

void Gfx_Fill(uint16_t *fb, int x, int y, int w, int h, uint16_t colour);
void Gfx_Frame(uint16_t *fb, int x, int y, int w, int h, uint16_t colour);

/** Halve the brightness of a rectangle - a cheap translucent backdrop. */
void Gfx_Darken(uint16_t *fb, int x, int y, int w, int h);

/**
  * @brief  Draw text with the built-in 5x7 font (6x8 cell per character at
  *         scale 1).  Lower case is drawn as upper case; unknown characters
  *         as spaces.
  * @retval Width drawn, in pixels.
  */
int Gfx_Text(uint16_t *fb, int x, int y, const char *s, uint16_t colour,
             int scale);

/** Width `s` would take at `scale`, for centring. */
int Gfx_TextWidth(const char *s, int scale);

/** 2x digital zoom about the frame centre, in place. */
void Gfx_Zoom2x(uint16_t *fb);

/** Paint pixels on strong edges (green-channel gradient) red. */
void Gfx_FocusPeaking(uint16_t *fb, uint32_t threshold);

/** Diagonal stripes over pixels whose luma is at or above `level` (0..63). */
void Gfx_Zebra(uint16_t *fb, uint32_t level, uint32_t phase);

/** Luma histogram, 64 bins, from a sparse sample of the frame. */
void Gfx_Histogram(const uint16_t *fb, uint32_t bins[64]);

/** Draw a histogram as white bars in a darkened box. */
void Gfx_DrawHistogram(uint16_t *fb, int x, int y, int h,
                       const uint32_t bins[64]);

/** Rule-of-thirds grid as dashed lines. */
void Gfx_Grid(uint16_t *fb);

#ifdef __cplusplus
}
#endif

#endif /* __GFX_H */
