/**
  ******************************************************************************
  * @file    gallery.c
  * @brief   Full-screen photo viewer, drawn in camera buffer 0.
  *
  *   left / right third of the picture   previous / next photo
  *   middle                              show or hide the captions
  *   top strip, left / right             25 photos back / forward
  *   bottom centre, or the shutter       back to the camera
  ******************************************************************************
  */

#include "gallery.h"
#include "camera.h"
#include "gfx.h"
#include "lcd.h"
#include "main.h"
#include "photo.h"
#include "sd.h"
#include "touch.h"
#include <stdio.h>
#include <string.h>

#define ACCENT   GFX_RGB(255, 150, 30)
#define GREY     0x8C71U

static void centred(uint16_t *fb, int y, const char *s, uint16_t c, int scale)
{
  (void)Gfx_Text(fb, (GFX_W - Gfx_TextWidth(s, scale)) / 2, y, s, c, scale);
}

static void captions(uint16_t *fb, int index, int count, const char *error)
{
  const char *name = strrchr(Photo_Path(index), '/');
  char        text[32];

  name = (name != 0) ? (name + 1) : Photo_Path(index);

  if (error != 0)
  {
    centred(fb, 112, error, ACCENT, 2);
  }

  Gfx_Panel(fb, 0, 0, GFX_W, 22);
  (void)snprintf(text, sizeof(text), "%.28s", name);
  (void)Gfx_Text(fb, 30, 8, text, LCD_WHITE, 1);
  (void)Gfx_Text(fb, 6, 8, "-25", GREY, 1);
  (void)Gfx_Text(fb, GFX_W - 24, 8, "+25", GREY, 1);
  (void)snprintf(text, sizeof(text), "%d/%d", index + 1, count);
  (void)Gfx_Text(fb, GFX_W - 34 - Gfx_TextWidth(text, 1), 8, text, ACCENT, 1);

  Gfx_Panel(fb, 6, 204, 58, 32);
  (void)Gfx_Text(fb, 30, 213, "<", LCD_WHITE, 2);
  Gfx_Panel(fb, 120, 204, 80, 32);
  centred(fb, 213, "BACK", LCD_WHITE, 2);
  Gfx_Panel(fb, 256, 204, 58, 32);
  (void)Gfx_Text(fb, 280, 213, ">", LCD_WHITE, 2);
}

static void show(uint16_t *fb)
{
  Lcd_BlitAsync(fb);
  Lcd_WaitIdle();
}

/** Wait for a tap. False when the shutter button or a timeout ends it. */
static bool wait_tap(int *x, int *y)
{
  Touch_Point_t pt;
  bool          down = false;

  for (;;)
  {
    if (HAL_GPIO_ReadPin(SHUTTER_GPIO_Port, SHUTTER_Pin) == GPIO_PIN_RESET)
    {
      HAL_Delay(40U);
      while (HAL_GPIO_ReadPin(SHUTTER_GPIO_Port, SHUTTER_Pin) == GPIO_PIN_RESET) { }
      HAL_Delay(40U);
      return false;
    }

    Touch_Read(&pt);

    if (pt.pressed)
    {
      down = true;
      *x   = pt.x;
      *y   = pt.y;
    }
    else if (down)
    {
      return true;
    }

    HAL_Delay(10U);
  }
}

void Gallery_Run(void)
{
  uint16_t   *fb       = (uint16_t *)Camera_GetFrameBuffer(0U);
  bool        labels   = true;
  bool        redraw   = true;
  const char *error    = 0;
  int         index;
  int         count;
  int         x = 0;
  int         y = 0;

  Lcd_WaitIdle();
  Gfx_Fill(fb, 0, 0, GFX_W, GFX_H, 0x0000U);
  centred(fb, 112, "READING CARD", LCD_WHITE, 2);
  show(fb);

  count = Photo_Scan();

  if (count <= 0)
  {
    Gfx_Fill(fb, 0, 0, GFX_W, GFX_H, 0x0000U);
    centred(fb, 100, (count < 0) ? "NO READABLE CARD" : "NO PHOTOS FOUND",
            ACCENT, 2);
    centred(fb, 132, "TAP TO GO BACK", GREY, 1);
    show(fb);
    (void)wait_tap(&x, &y);
    return;
  }

  /* Start at the newest photo taken here, else at the first on the card. */
  index = (Photo_OwnCount() > 0) ? (Photo_OwnCount() - 1) : 0;

  for (;;)
  {
    if (redraw)
    {
      /* Large JPEGs take a moment: say so over the old picture. */
      Gfx_Panel(fb, 110, 106, 100, 28);
      centred(fb, 116, "LOADING", LCD_WHITE, 1);
      show(fb);

      error = Photo_Show(index, fb);

      if (labels || (error != 0)) { captions(fb, index, count, error); }
      show(fb);
      redraw = false;
    }

    if (!wait_tap(&x, &y))
    {
      return;
    }

    if ((y >= 196) && (x >= 110) && (x < 210))
    {
      return;
    }

    redraw = true;

    int step = (y < 30) ? (25 % count) : 1;

    if (x < (GFX_W / 3))
    {
      index = (index + count - step) % count;
    }
    else if (x >= ((2 * GFX_W) / 3))
    {
      index = (index + step) % count;
    }
    else
    {
      labels = !labels;
    }
  }
}
