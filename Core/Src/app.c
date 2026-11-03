/**
  ******************************************************************************
  * @file    app.c
  * @brief   Camera application: touch menu, status bar, picture controls,
  *          shooting aids and freeze/review on top of the live preview.
  ******************************************************************************
  */

#include "app.h"
#include "gfx.h"
#include "lcd.h"
#include "touch.h"
#include "ov5640.h"
#include "main.h"
#include <stdio.h>

/* -------------------------------------------------------------------------- */
/* Settings                                                                   */
/* -------------------------------------------------------------------------- */

typedef struct
{
  OV5640_Effect_t       effect;
  OV5640_WhiteBalance_t wb;
  int8_t                ev;
  uint8_t               orient;   /* 0 std, 1 mirror, 2 flip, 3 rotate 180 */
  bool                  grid;
  bool                  hist;
  bool                  zebra;
  bool                  peak;
  bool                  zoom;
  bool                  night;
  bool                  timer;
  bool                  draw;
} App_Settings_t;

static App_Settings_t app;

static const char *const effect_names[OV5640_EFFECT_COUNT] =
  { "STD", "B&W", "SEPIA", "NEG", "RED", "GREEN", "BLUE" };

static const char *const wb_names[OV5640_WB_COUNT] =
  { "AUTO", "SUN", "CLOUD", "OFFICE", "HOME" };

static const char *const ev_names[7] =
  { "-1.0", "-0.7", "-0.3", "0.0", "+0.3", "+0.7", "+1.0" };

static const char *const orient_names[4] =
  { "STD", "MIRROR", "FLIP", "ROT180" };

/* The sensor table starts mirrored (0x3821 = 0x07), which is "STD" here. */
static void app_apply_orient(void)
{
  static const bool mirror[4] = { true, false, true,  false };
  static const bool flip[4]   = { false, false, true, true  };

  (void)OV5640_SetMirrorFlip(mirror[app.orient], flip[app.orient]);
}

/* -------------------------------------------------------------------------- */
/* UI state                                                                   */
/* -------------------------------------------------------------------------- */

#define COLOUR_YELLOW    0xFFE0U
#define COLOUR_GREY      0x8410U
#define COLOUR_ACCENT    0xFD20U        /* orange */
#define COLOUR_ON        0x07E0U

#define TOUCH_POLL_MS    10U
#define TRAIL_LEN        160U
#define TRAIL_MS         1500U
#define TIMER_MS         3000U
#define FLASH_MS         90U

typedef enum
{
  MI_EFFECT, MI_WB,    MI_EV_DOWN, MI_EV_UP,
  MI_GRID,   MI_HIST,  MI_ZEBRA,   MI_PEAK,
  MI_ZOOM,   MI_ORIENT, MI_NIGHT,  MI_TIMER,
  MI_DRAW,   MI_CAL,   MI_SHOT,    MI_CLOSE,
  MI_COUNT
} Menu_Item_t;

static const char *const menu_titles[MI_COUNT] =
{
  "EFFECT", "WB",     "EV -",  "EV +",
  "GRID",   "HISTO",  "ZEBRA", "PEAKING",
  "ZOOM",   "ORIENT", "NIGHT", "TIMER",
  "DRAW",   "TOUCH",  "SHOOT", "CLOSE",
};

/* Menu: 4 x 4 buttons over the live image. */
#define MENU_X0     10
#define MENU_Y0     22
#define MENU_BW     72
#define MENU_BH     46
#define MENU_GAP    4

/* Buttons shown while the menu is closed. */
#define SHOT_X      4
#define SHOT_Y      204
#define SHOT_W      64
#define SHOT_H      32
#define MENU_BTN_X  252

typedef struct
{
  int16_t  x;
  int16_t  y;
  uint32_t t;
} Trail_Point_t;

/* Latest touch reading, handy to watch over SWD. */
typedef struct
{
  uint32_t presses;
  uint16_t raw_x, raw_y, raw_z;
  int16_t  x, y;
  uint8_t  pressed;
} Touch_Stats_t;

volatile Touch_Stats_t touch_stats;

static bool          menu_open;
static bool          frozen;            /* reviewing a captured frame     */
static bool          freeze_shown;      /* ...and it is already on screen */
static uint32_t      flash_until;       /* white flash after the shutter  */
static uint32_t      timer_deadline;    /* self-timer running if non-zero */
static bool          cal_request;

static uint32_t      fps_camera;
static uint32_t      fps_display;
static uint32_t      shots;

static Touch_Point_t touch_now;
static bool          press_on_control;  /* press began on a button        */
static Trail_Point_t trail[TRAIL_LEN];
static uint32_t      trail_head;

/* -------------------------------------------------------------------------- */
/* Layout helpers                                                             */
/* -------------------------------------------------------------------------- */

static bool in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
  return (x >= rx) && (x < (rx + rw)) && (y >= ry) && (y < (ry + rh));
}

static void menu_rect(int item, int *x, int *y)
{
  *x = MENU_X0 + ((item % 4) * (MENU_BW + MENU_GAP));
  *y = MENU_Y0 + ((item / 4) * (MENU_BH + MENU_GAP));
}

static int menu_hit(int x, int y)
{
  for (int i = 0; i < MI_COUNT; i++)
  {
    int bx;
    int by;

    menu_rect(i, &bx, &by);

    if (in_rect(x, y, bx, by, MENU_BW, MENU_BH))
    {
      return i;
    }
  }

  return -1;
}

/** Current value of a menu item, as shown under its title. */
static const char *menu_value(int item)
{
  switch (item)
  {
    case MI_EFFECT: return effect_names[app.effect];
    case MI_WB:     return wb_names[app.wb];
    case MI_EV_DOWN:
    case MI_EV_UP:  return ev_names[app.ev - OV5640_EV_MIN];
    case MI_GRID:   return app.grid  ? "ON" : "OFF";
    case MI_HIST:   return app.hist  ? "ON" : "OFF";
    case MI_ZEBRA:  return app.zebra ? "ON" : "OFF";
    case MI_PEAK:   return app.peak  ? "ON" : "OFF";
    case MI_ZOOM:   return app.zoom  ? "2X" : "1X";
    case MI_ORIENT: return orient_names[app.orient];
    case MI_NIGHT:  return app.night ? "ON" : "OFF";
    case MI_TIMER:  return app.timer ? "3S" : "OFF";
    case MI_DRAW:   return app.draw  ? "ON" : "OFF";
    case MI_CAL:    return "CALIB";
    case MI_SHOT:   return "";
    default:        return "";
  }
}

static bool menu_is_on(int item)
{
  switch (item)
  {
    case MI_GRID:  return app.grid;
    case MI_HIST:  return app.hist;
    case MI_ZEBRA: return app.zebra;
    case MI_PEAK:  return app.peak;
    case MI_ZOOM:  return app.zoom;
    case MI_NIGHT: return app.night;
    case MI_TIMER: return app.timer;
    case MI_DRAW:  return app.draw;
    default:       return false;
  }
}

/* -------------------------------------------------------------------------- */
/* Actions                                                                    */
/* -------------------------------------------------------------------------- */

static void app_shoot(void)
{
  /* Flash first, then freeze on the next frame and hold it for review. */
  uint32_t now = HAL_GetTick();

  shots++;
  frozen       = true;
  freeze_shown = false;
  flash_until  = now + FLASH_MS;
  menu_open    = false;

  Lcd_FillAsync(LCD_WHITE);
}

static void app_shutter(void)
{
  if (frozen)
  {
    frozen = false;
    return;
  }

  if (timer_deadline != 0U)
  {
    timer_deadline = 0U;            /* second press cancels the countdown */
    return;
  }

  if (app.timer)
  {
    timer_deadline = HAL_GetTick() + TIMER_MS;
    menu_open      = false;
  }
  else
  {
    app_shoot();
  }
}

static void menu_select(int item)
{
  switch (item)
  {
    case MI_EFFECT:
      app.effect = (OV5640_Effect_t)((app.effect + 1) % OV5640_EFFECT_COUNT);
      (void)OV5640_SetEffect(app.effect);
      break;

    case MI_WB:
      app.wb = (OV5640_WhiteBalance_t)((app.wb + 1) % OV5640_WB_COUNT);
      (void)OV5640_SetWhiteBalance(app.wb);
      break;

    case MI_EV_DOWN:
      if (app.ev > OV5640_EV_MIN) { app.ev--; }
      (void)OV5640_SetExposureComp(app.ev);
      break;

    case MI_EV_UP:
      if (app.ev < OV5640_EV_MAX) { app.ev++; }
      (void)OV5640_SetExposureComp(app.ev);
      break;

    case MI_GRID:   app.grid  = !app.grid;  break;
    case MI_HIST:   app.hist  = !app.hist;  break;
    case MI_ZEBRA:  app.zebra = !app.zebra; break;
    case MI_PEAK:   app.peak  = !app.peak;  break;
    case MI_ZOOM:   app.zoom  = !app.zoom;  break;
    case MI_TIMER:  app.timer = !app.timer; break;
    case MI_DRAW:   app.draw  = !app.draw;  break;

    case MI_ORIENT:
      app.orient = (uint8_t)((app.orient + 1U) % 4U);
      app_apply_orient();
      break;

    case MI_NIGHT:
      app.night = !app.night;
      (void)OV5640_SetNightMode(app.night);
      break;

    case MI_CAL:
      cal_request = true;
      menu_open   = false;
      break;

    case MI_SHOT:
      menu_open = false;
      app_shutter();
      break;

    case MI_CLOSE:
    default:
      menu_open = false;
      break;
  }
}

/** A finger was lifted; (x, y) is where it last was. */
static void app_tap(int x, int y)
{
  if (frozen)
  {
    frozen = false;
    return;
  }

  if (menu_open)
  {
    int item = menu_hit(x, y);

    if (item >= 0)
    {
      menu_select(item);
    }
    else
    {
      menu_open = false;              /* tap outside the grid closes it */
    }

    return;
  }

  if (in_rect(x, y, MENU_BTN_X, SHOT_Y, SHOT_W, SHOT_H))
  {
    menu_open = true;
  }
  else if (in_rect(x, y, SHOT_X, SHOT_Y, SHOT_W, SHOT_H))
  {
    app_shutter();
  }
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

void App_Init(void)
{
  app.effect = OV5640_EFFECT_NORMAL;
  app.wb     = OV5640_WB_AUTO;
  app.ev     = 0;
  app.orient = 0U;
  app.grid   = false;
  app.hist   = false;
  app.zebra  = false;
  app.peak   = false;
  app.zoom   = false;
  app.night  = false;
  app.timer  = false;
  app.draw   = true;

  (void)OV5640_SetEffect(app.effect);
  (void)OV5640_SetWhiteBalance(app.wb);
  (void)OV5640_SetExposureComp(app.ev);
  (void)OV5640_SetNightMode(app.night);
}

void App_Poll(void)
{
  static uint32_t last_poll;
  static bool     was_pressed;
  static bool     shutter_was_down;
  static uint32_t shutter_change;
  uint32_t        now = HAL_GetTick();
  bool            shutter_down;

  /* Physical shutter on PF14, active low, debounced. */
  shutter_down = (HAL_GPIO_ReadPin(SHUTTER_GPIO_Port, SHUTTER_Pin)
                  == GPIO_PIN_RESET);

  if ((shutter_down != shutter_was_down) && ((now - shutter_change) > 40U))
  {
    shutter_was_down = shutter_down;
    shutter_change   = now;

    if (shutter_down)
    {
      app_shutter();
    }
  }

  if ((timer_deadline != 0U) && ((int32_t)(now - timer_deadline) >= 0))
  {
    timer_deadline = 0U;
    app_shoot();
  }

  if (Lcd_IsBusy() || ((now - last_poll) < TOUCH_POLL_MS))
  {
    return;
  }

  last_poll = now;

  Touch_Read(&touch_now);

  if (touch_now.pressed)
  {
    if (!was_pressed)
    {
      touch_stats.presses++;

      /* Strokes that start on a control or with the menu up are taps, not
       * drawing. */
      press_on_control = menu_open || frozen ||
        in_rect(touch_now.x, touch_now.y, SHOT_X, SHOT_Y, SHOT_W, SHOT_H) ||
        in_rect(touch_now.x, touch_now.y, MENU_BTN_X, SHOT_Y, SHOT_W, SHOT_H);
    }

    if (app.draw && !press_on_control)
    {
      const Trail_Point_t *prev =
          &trail[(trail_head + TRAIL_LEN - 1U) % TRAIL_LEN];
      int32_t dx = touch_now.x - prev->x;
      int32_t dy = touch_now.y - prev->y;

      if (!was_pressed || ((dx * dx) + (dy * dy) >= 4))
      {
        trail[trail_head].x = touch_now.x;
        trail[trail_head].y = touch_now.y;
        trail[trail_head].t = now;
        trail_head = (trail_head + 1U) % TRAIL_LEN;
      }
    }

    touch_stats.raw_x = touch_now.raw_x;
    touch_stats.raw_y = touch_now.raw_y;
    touch_stats.raw_z = touch_now.raw_z;
    touch_stats.x     = touch_now.x;
    touch_stats.y     = touch_now.y;
  }
  else if (was_pressed && (press_on_control || !app.draw))
  {
    /* A drawing stroke that happens to end on a button is not a tap. */
    app_tap(touch_stats.x, touch_stats.y);
  }

  was_pressed         = touch_now.pressed;
  touch_stats.pressed = touch_now.pressed ? 1U : 0U;
}

bool App_WantsFrame(void)
{
  if ((int32_t)(HAL_GetTick() - flash_until) < 0)
  {
    return false;                     /* white flash still on screen */
  }

  return !(frozen && freeze_shown);
}

static void draw_status(uint16_t *fb)
{
  char text[64];

  Gfx_Darken(fb, 0, 0, GFX_W, 14);

  (void)snprintf(text, sizeof(text), "%s  WB %s  EV %s%s%s",
                 effect_names[app.effect], wb_names[app.wb],
                 ev_names[app.ev - OV5640_EV_MIN],
                 app.zoom  ? "  2X"    : "",
                 app.night ? "  NIGHT" : "");
  (void)Gfx_Text(fb, 4, 3, text, LCD_WHITE, 1);

  (void)snprintf(text, sizeof(text), "%lu FPS", (unsigned long)fps_display);
  (void)Gfx_Text(fb, GFX_W - 4 - Gfx_TextWidth(text, 1), 3, text,
                 COLOUR_ACCENT, 1);
}

static void draw_button(uint16_t *fb, int x, int y, int w, int h,
                        const char *title, const char *value, bool on)
{
  Gfx_Darken(fb, x, y, w, h);
  Gfx_Frame(fb, x, y, w, h, on ? COLOUR_ON : COLOUR_GREY);

  if ((value != NULL) && (value[0] != '\0'))
  {
    (void)Gfx_Text(fb, x + ((w - Gfx_TextWidth(title, 1)) / 2),
                   y + (h / 2) - 10, title, LCD_WHITE, 1);
    (void)Gfx_Text(fb, x + ((w - Gfx_TextWidth(value, 1)) / 2),
                   y + (h / 2) + 3, value, on ? COLOUR_ON : COLOUR_ACCENT, 1);
  }
  else
  {
    (void)Gfx_Text(fb, x + ((w - Gfx_TextWidth(title, 1)) / 2),
                   y + (h / 2) - 3, title, LCD_WHITE, 1);
  }
}

static void draw_menu(uint16_t *fb)
{
  for (int i = 0; i < MI_COUNT; i++)
  {
    int x;
    int y;

    menu_rect(i, &x, &y);
    draw_button(fb, x, y, MENU_BW, MENU_BH, menu_titles[i], menu_value(i),
                menu_is_on(i));
  }
}

static void draw_trail(uint16_t *fb)
{
  uint32_t now = HAL_GetTick();

  for (uint32_t i = 0U; i < TRAIL_LEN; i++)
  {
    if ((trail[i].t != 0U) && ((now - trail[i].t) < TRAIL_MS))
    {
      Gfx_Fill(fb, trail[i].x - 1, trail[i].y - 1, 3, 3, COLOUR_YELLOW);
    }
  }

  if (touch_now.pressed && !press_on_control)
  {
    Gfx_Fill(fb, touch_now.x - 5, touch_now.y - 5, 11, 11, LCD_WHITE);
    Gfx_Fill(fb, touch_now.x - 4, touch_now.y - 4, 9, 9, LCD_RED);
  }
}

void App_ProcessFrame(uint16_t *frame)
{
  static uint32_t bins[64];
  uint32_t        now = HAL_GetTick();

  /* Image tools first, on the picture itself... */
  if (app.zoom)
  {
    Gfx_Zoom2x(frame);
  }

  if (app.hist)
  {
    Gfx_Histogram(frame, bins);
  }

  if (app.peak)
  {
    Gfx_FocusPeaking(frame, 14U);
  }

  if (app.zebra)
  {
    Gfx_Zebra(frame, 60U, now / 60U);
  }

  if (app.grid)
  {
    Gfx_Grid(frame);
  }

  /* ...then the interface on top. */
  if (frozen)
  {
    const char *msg = "REVIEW - TAP TO RESUME";
    char        count[24];

    Gfx_Darken(frame, 0, GFX_H - 20, GFX_W, 20);
    (void)Gfx_Text(frame, (GFX_W - Gfx_TextWidth(msg, 1)) / 2, GFX_H - 14,
                   msg, LCD_WHITE, 1);
    (void)snprintf(count, sizeof(count), "SHOT %lu", (unsigned long)shots);
    Gfx_Darken(frame, 0, 0, GFX_W, 14);
    (void)Gfx_Text(frame, 4, 3, count, COLOUR_ACCENT, 1);
    freeze_shown = true;
    return;
  }

  if (app.draw && !menu_open)
  {
    draw_trail(frame);
  }

  if (app.hist)
  {
    Gfx_DrawHistogram(frame, 8, 160, 36, bins);
  }

  draw_status(frame);

  if (menu_open)
  {
    draw_menu(frame);
  }
  else
  {
    draw_button(frame, SHOT_X, SHOT_Y, SHOT_W, SHOT_H,
                app.timer ? "TIMER" : "SHOOT", NULL, false);
    draw_button(frame, MENU_BTN_X, SHOT_Y, SHOT_W, SHOT_H, "MENU", NULL,
                false);
  }

  if (timer_deadline != 0U)
  {
    char     digit[2];
    uint32_t left = (timer_deadline - now + 999U) / 1000U;

    digit[0] = (char)('0' + (left > 9U ? 9U : left));
    digit[1] = '\0';
    (void)Gfx_Text(frame, (GFX_W - Gfx_TextWidth(digit, 8)) / 2,
                   (GFX_H - 56) / 2, digit, COLOUR_ACCENT, 8);
  }
}

void App_SetFps(uint32_t camera_fps, uint32_t display_fps)
{
  fps_camera  = camera_fps;
  fps_display = display_fps;
  (void)fps_camera;
}

bool App_TakeCalibrationRequest(void)
{
  bool request = cal_request;

  cal_request = false;

  return request;
}
