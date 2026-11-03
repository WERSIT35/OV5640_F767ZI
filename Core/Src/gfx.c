/**
  ******************************************************************************
  * @file    gfx.c
  * @brief   Frame-buffer drawing and per-frame image tools.
  ******************************************************************************
  */

/* The Debug configuration builds at -O0, which makes these whole-frame pixel
 * loops several times slower than they need to be on the live path.  Keep
 * this one file optimised regardless of the project setting. */
#pragma GCC optimize ("O2")

#include "gfx.h"

/* -------------------------------------------------------------------------- */
/* 5x7 font, ASCII 32..95.  Five column bytes per glyph, bit 0 = top row.     */
/* -------------------------------------------------------------------------- */

static const uint8_t gfx_font[64][5] =
{
  { 0x00, 0x00, 0x00, 0x00, 0x00 }, /* ' ' */
  { 0x00, 0x00, 0x5F, 0x00, 0x00 }, /* '!' */
  { 0x00, 0x07, 0x00, 0x07, 0x00 }, /* '"' */
  { 0x14, 0x7F, 0x14, 0x7F, 0x14 }, /* '#' */
  { 0x24, 0x2A, 0x7F, 0x2A, 0x12 }, /* '$' */
  { 0x23, 0x13, 0x08, 0x64, 0x62 }, /* '%' */
  { 0x36, 0x49, 0x56, 0x20, 0x50 }, /* '&' */
  { 0x00, 0x08, 0x07, 0x03, 0x00 }, /* ''' */
  { 0x00, 0x1C, 0x22, 0x41, 0x00 }, /* '(' */
  { 0x00, 0x41, 0x22, 0x1C, 0x00 }, /* ')' */
  { 0x2A, 0x1C, 0x7F, 0x1C, 0x2A }, /* '*' */
  { 0x08, 0x08, 0x3E, 0x08, 0x08 }, /* '+' */
  { 0x00, 0x50, 0x30, 0x00, 0x00 }, /* ',' */
  { 0x08, 0x08, 0x08, 0x08, 0x08 }, /* '-' */
  { 0x00, 0x60, 0x60, 0x00, 0x00 }, /* '.' */
  { 0x20, 0x10, 0x08, 0x04, 0x02 }, /* '/' */
  { 0x3E, 0x51, 0x49, 0x45, 0x3E }, /* '0' */
  { 0x00, 0x42, 0x7F, 0x40, 0x00 }, /* '1' */
  { 0x42, 0x61, 0x51, 0x49, 0x46 }, /* '2' */
  { 0x21, 0x41, 0x45, 0x4B, 0x31 }, /* '3' */
  { 0x18, 0x14, 0x12, 0x7F, 0x10 }, /* '4' */
  { 0x27, 0x45, 0x45, 0x45, 0x39 }, /* '5' */
  { 0x3C, 0x4A, 0x49, 0x49, 0x30 }, /* '6' */
  { 0x01, 0x71, 0x09, 0x05, 0x03 }, /* '7' */
  { 0x36, 0x49, 0x49, 0x49, 0x36 }, /* '8' */
  { 0x06, 0x49, 0x49, 0x29, 0x1E }, /* '9' */
  { 0x00, 0x36, 0x36, 0x00, 0x00 }, /* ':' */
  { 0x00, 0x56, 0x36, 0x00, 0x00 }, /* ';' */
  { 0x08, 0x14, 0x22, 0x41, 0x00 }, /* '<' */
  { 0x14, 0x14, 0x14, 0x14, 0x14 }, /* '=' */
  { 0x00, 0x41, 0x22, 0x14, 0x08 }, /* '>' */
  { 0x02, 0x01, 0x59, 0x09, 0x06 }, /* '?' */
  { 0x3E, 0x41, 0x5D, 0x59, 0x4E }, /* '@' */
  { 0x7E, 0x11, 0x11, 0x11, 0x7E }, /* 'A' */
  { 0x7F, 0x49, 0x49, 0x49, 0x36 }, /* 'B' */
  { 0x3E, 0x41, 0x41, 0x41, 0x22 }, /* 'C' */
  { 0x7F, 0x41, 0x41, 0x22, 0x1C }, /* 'D' */
  { 0x7F, 0x49, 0x49, 0x49, 0x41 }, /* 'E' */
  { 0x7F, 0x09, 0x09, 0x09, 0x01 }, /* 'F' */
  { 0x3E, 0x41, 0x49, 0x49, 0x7A }, /* 'G' */
  { 0x7F, 0x08, 0x08, 0x08, 0x7F }, /* 'H' */
  { 0x00, 0x41, 0x7F, 0x41, 0x00 }, /* 'I' */
  { 0x20, 0x40, 0x41, 0x3F, 0x01 }, /* 'J' */
  { 0x7F, 0x08, 0x14, 0x22, 0x41 }, /* 'K' */
  { 0x7F, 0x40, 0x40, 0x40, 0x40 }, /* 'L' */
  { 0x7F, 0x02, 0x0C, 0x02, 0x7F }, /* 'M' */
  { 0x7F, 0x04, 0x08, 0x10, 0x7F }, /* 'N' */
  { 0x3E, 0x41, 0x41, 0x41, 0x3E }, /* 'O' */
  { 0x7F, 0x09, 0x09, 0x09, 0x06 }, /* 'P' */
  { 0x3E, 0x41, 0x51, 0x21, 0x5E }, /* 'Q' */
  { 0x7F, 0x09, 0x19, 0x29, 0x46 }, /* 'R' */
  { 0x46, 0x49, 0x49, 0x49, 0x31 }, /* 'S' */
  { 0x01, 0x01, 0x7F, 0x01, 0x01 }, /* 'T' */
  { 0x3F, 0x40, 0x40, 0x40, 0x3F }, /* 'U' */
  { 0x1F, 0x20, 0x40, 0x20, 0x1F }, /* 'V' */
  { 0x3F, 0x40, 0x38, 0x40, 0x3F }, /* 'W' */
  { 0x63, 0x14, 0x08, 0x14, 0x63 }, /* 'X' */
  { 0x07, 0x08, 0x70, 0x08, 0x07 }, /* 'Y' */
  { 0x61, 0x51, 0x49, 0x45, 0x43 }, /* 'Z' */
  { 0x00, 0x7F, 0x41, 0x41, 0x00 }, /* '[' */
  { 0x02, 0x04, 0x08, 0x10, 0x20 }, /* '\' */
  { 0x00, 0x41, 0x41, 0x7F, 0x00 }, /* ']' */
  { 0x04, 0x02, 0x01, 0x02, 0x04 }, /* '^' */
  { 0x40, 0x40, 0x40, 0x40, 0x40 }, /* '_' */
};

/* -------------------------------------------------------------------------- */
/* Primitives                                                                 */
/* -------------------------------------------------------------------------- */

/** Clip a rectangle to the frame. Returns false if nothing is left. */
static bool gfx_clip(int *x, int *y, int *w, int *h)
{
  if (*x < 0) { *w += *x; *x = 0; }
  if (*y < 0) { *h += *y; *y = 0; }
  if ((*x + *w) > GFX_W) { *w = GFX_W - *x; }
  if ((*y + *h) > GFX_H) { *h = GFX_H - *y; }

  return (*w > 0) && (*h > 0);
}

void Gfx_Fill(uint16_t *fb, int x, int y, int w, int h, uint16_t colour)
{
  if (!gfx_clip(&x, &y, &w, &h))
  {
    return;
  }

  for (int yy = y; yy < (y + h); yy++)
  {
    uint16_t *row = &fb[yy * GFX_W];

    for (int xx = x; xx < (x + w); xx++)
    {
      row[xx] = colour;
    }
  }
}

void Gfx_Frame(uint16_t *fb, int x, int y, int w, int h, uint16_t colour)
{
  Gfx_Fill(fb, x, y, w, 1, colour);
  Gfx_Fill(fb, x, y + h - 1, w, 1, colour);
  Gfx_Fill(fb, x, y, 1, h, colour);
  Gfx_Fill(fb, x + w - 1, y, 1, h, colour);
}

void Gfx_Darken(uint16_t *fb, int x, int y, int w, int h)
{
  if (!gfx_clip(&x, &y, &w, &h))
  {
    return;
  }

  for (int yy = y; yy < (y + h); yy++)
  {
    uint16_t *row = &fb[yy * GFX_W];

    for (int xx = x; xx < (x + w); xx++)
    {
      /* Halve each channel: shift, then drop the bits that crossed fields. */
      row[xx] = (uint16_t)((row[xx] >> 1) & 0x7BEFU);
    }
  }
}

static void gfx_char(uint16_t *fb, int x, int y, char c, uint16_t colour,
                     int scale)
{
  const uint8_t *glyph;

  if ((c >= 'a') && (c <= 'z'))
  {
    c = (char)(c - 'a' + 'A');
  }

  if ((c < ' ') || (c > '_'))
  {
    c = ' ';
  }

  glyph = gfx_font[(uint8_t)c - (uint8_t)' '];

  for (int col = 0; col < 5; col++)
  {
    uint8_t bits = glyph[col];

    for (int row = 0; row < 7; row++)
    {
      if ((bits & (1U << row)) != 0U)
      {
        Gfx_Fill(fb, x + (col * scale), y + (row * scale), scale, scale,
                 colour);
      }
    }
  }
}

int Gfx_Text(uint16_t *fb, int x, int y, const char *s, uint16_t colour,
             int scale)
{
  int start = x;

  while (*s != '\0')
  {
    gfx_char(fb, x, y, *s++, colour, scale);
    x += 6 * scale;
  }

  return x - start;
}

int Gfx_TextWidth(const char *s, int scale)
{
  int n = 0;

  while (s[n] != '\0')
  {
    n++;
  }

  return (n * 6 * scale) - scale;
}

/* -------------------------------------------------------------------------- */
/* Image tools                                                                */
/* -------------------------------------------------------------------------- */

static inline uint32_t gfx_luma6(uint16_t p)
{
  uint32_t r = (p >> 11) & 0x1FU;
  uint32_t g = (p >> 5)  & 0x3FU;
  uint32_t b = p & 0x1FU;

  /* BT.601 weights on a common 6-bit scale: 0..63. */
  return ((r * 2U * 77U) + (g * 150U) + (b * 2U * 29U)) >> 8;
}

void Gfx_Zoom2x(uint16_t *fb)
{
  /* In place, using one line of scratch.  The top half is written top-down
   * and the bottom half bottom-up, so every source row (60..179) is read
   * before anything overwrites it. */
  static uint16_t line[GFX_W / 2];

  for (int pass = 0; pass < 2; pass++)
  {
    for (int i = 0; i < (GFX_H / 2); i++)
    {
      int y  = (pass == 0) ? i : (GFX_H - 1 - i);
      int sy = (GFX_H / 4) + (y / 2);
      const uint16_t *src = &fb[(sy * GFX_W) + (GFX_W / 4)];
      uint16_t       *dst = &fb[y * GFX_W];

      for (int x = 0; x < (GFX_W / 2); x++)
      {
        line[x] = src[x];
      }

      for (int x = 0; x < GFX_W; x++)
      {
        dst[x] = line[x / 2];
      }
    }
  }
}

void Gfx_FocusPeaking(uint16_t *fb, uint32_t threshold)
{
  /* Green channel of the previous, current and next rows, copied out first
   * so painted pixels never feed back into the gradient. */
  static uint8_t prev[GFX_W];
  static uint8_t cur[GFX_W];
  static uint8_t next[GFX_W];

  for (int x = 0; x < GFX_W; x++)
  {
    cur[x]  = (uint8_t)((fb[x] >> 5) & 0x3FU);
    next[x] = (uint8_t)((fb[GFX_W + x] >> 5) & 0x3FU);
    prev[x] = cur[x];
  }

  for (int y = 1; y < (GFX_H - 1); y++)
  {
    uint16_t *row = &fb[y * GFX_W];

    for (int x = 0; x < GFX_W; x++)
    {
      prev[x] = cur[x];
      cur[x]  = next[x];
      next[x] = (uint8_t)((fb[((y + 1) * GFX_W) + x] >> 5) & 0x3FU);
    }

    for (int x = 1; x < (GFX_W - 1); x++)
    {
      int32_t gx = (int32_t)cur[x + 1] - (int32_t)cur[x - 1];
      int32_t gy = (int32_t)next[x] - (int32_t)prev[x];

      if (gx < 0) { gx = -gx; }
      if (gy < 0) { gy = -gy; }

      if ((uint32_t)(gx + gy) >= threshold)
      {
        row[x] = 0xF800U;
      }
    }
  }
}

void Gfx_Zebra(uint16_t *fb, uint32_t level, uint32_t phase)
{
  for (int y = 0; y < GFX_H; y++)
  {
    uint16_t *row = &fb[y * GFX_W];

    for (int x = 0; x < GFX_W; x++)
    {
      if ((((uint32_t)(x + y) + phase) & 7U) < 3U)
      {
        if (gfx_luma6(row[x]) >= level)
        {
          row[x] = 0x0000U;
        }
      }
    }
  }
}

void Gfx_Histogram(const uint16_t *fb, uint32_t bins[64])
{
  for (int i = 0; i < 64; i++)
  {
    bins[i] = 0U;
  }

  for (int y = 0; y < GFX_H; y += 2)
  {
    const uint16_t *row = &fb[y * GFX_W];

    for (int x = 0; x < GFX_W; x += 4)
    {
      bins[gfx_luma6(row[x])]++;
    }
  }
}

void Gfx_DrawHistogram(uint16_t *fb, int x, int y, int h,
                       const uint32_t bins[64])
{
  uint32_t peak = 1U;

  for (int i = 0; i < 64; i++)
  {
    if (bins[i] > peak) { peak = bins[i]; }
  }

  Gfx_Darken(fb, x - 2, y - 2, 64 + 4, h + 4);
  Gfx_Frame(fb, x - 2, y - 2, 64 + 4, h + 4, 0x8410U);

  for (int i = 0; i < 64; i++)
  {
    int bar = (int)((bins[i] * (uint32_t)h) / peak);

    Gfx_Fill(fb, x + i, y + h - bar, 1, bar, 0xFFFFU);
  }
}

void Gfx_Grid(uint16_t *fb)
{
  static const int xs[2] = { GFX_W / 3, (2 * GFX_W) / 3 };
  static const int ys[2] = { GFX_H / 3, (2 * GFX_H) / 3 };

  for (int i = 0; i < 2; i++)
  {
    for (int y = 0; y < GFX_H; y++)
    {
      if ((y & 4) == 0) { fb[(y * GFX_W) + xs[i]] = 0xC618U; }
    }

    for (int x = 0; x < GFX_W; x++)
    {
      if ((x & 4) == 0) { fb[(ys[i] * GFX_W) + x] = 0xC618U; }
    }
  }
}
