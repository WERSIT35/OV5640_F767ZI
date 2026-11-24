/**
  ******************************************************************************
  * @file    splash.c
  * @brief   Start-up screen.  Draws into camera buffer 0, which is free until
  *          live preview starts.
  ******************************************************************************
  */

#pragma GCC optimize ("O2")

#include "splash.h"
#include "gfx.h"
#include "lcd.h"
#include "camera.h"

#define SP_BG      GFX_RGB(8, 10, 20)
#define SP_RING    GFX_RGB(200, 205, 215)
#define SP_DARK    GFX_RGB(24, 28, 44)
#define SP_ACCENT  GFX_RGB(255, 150, 30)
#define SP_GLASS1  GFX_RGB(30, 70, 150)
#define SP_GLASS2  GFX_RGB(60, 140, 220)

#define SP_CX      (GFX_W / 2)
#define SP_CY      84

static void splash_draw(int iris, int title, const char *text, int percent)
{
  static const char name[] = "OV5640 CAM";
  uint16_t *fb = (uint16_t *)Camera_GetFrameBuffer(0U);
  int       w  = Gfx_TextWidth(name, 3);

  Lcd_WaitIdle();
  Gfx_Fill(fb, 0, 0, GFX_W, GFX_H, SP_BG);

  Gfx_Disc(fb, SP_CX, SP_CY, 56, SP_RING);
  Gfx_Disc(fb, SP_CX, SP_CY, 53, SP_DARK);
  Gfx_Disc(fb, SP_CX, SP_CY, 46, SP_ACCENT);
  Gfx_Disc(fb, SP_CX, SP_CY, 44, 0x0000U);
  Gfx_Disc(fb, SP_CX, SP_CY, iris, SP_GLASS1);
  Gfx_Disc(fb, SP_CX, SP_CY, (iris * 2) / 3, SP_GLASS2);
  Gfx_Disc(fb, SP_CX, SP_CY, iris / 3, 0x0000U);
  Gfx_Disc(fb, SP_CX - (iris / 2), SP_CY - (iris / 2), iris / 7, LCD_WHITE);

  if (title > 0)
  {
    (void)Gfx_Text(fb, (GFX_W - w) / 2, 156, name, LCD_WHITE, 3);
    Gfx_Fill(fb, SP_CX - ((w * title) / 20), 184, (w * title) / 10, 2,
             SP_ACCENT);
  }

  if (text != 0)
  {
    (void)Gfx_Text(fb, (GFX_W - Gfx_TextWidth(text, 1)) / 2, 198, text,
                   SP_RING, 1);
    Gfx_FillRound(fb, 60, 214, 200, 8, SP_DARK);

    if (percent > 4)
    {
      Gfx_FillRound(fb, 60, 214, 2 * percent, 8, SP_ACCENT);
    }
  }

  Lcd_BlitAsync(fb);
  Lcd_WaitIdle();
}

void Splash_Intro(void)
{
  /* Iris closed, then opening with a soft stop; the title follows it. */
  for (int i = 0; i <= 24; i++)
  {
    int left  = 24 - i;
    int title = i - 12;

    if (title < 0)  { title = 0;  }
    if (title > 10) { title = 10; }

    splash_draw(42 - ((36 * left * left) / (24 * 24)), title, 0, 0);
  }
}

void Splash_Step(const char *text, int percent)
{
  splash_draw(42, 10, text, percent);
}
