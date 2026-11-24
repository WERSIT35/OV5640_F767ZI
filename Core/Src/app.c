/**
  ******************************************************************************
  * @file    app.c
  * @brief   Camera application: touch UI, picture controls, shooting aids and
  *          freeze/review on top of the live preview.
  *
  *   top     status bar: picture settings left, power and storage right
  *   bottom  menu button, round shutter, zoom button
  *   menu    three tabs of six labelled tiles
  *   tap on the picture hides or shows the interface
  ******************************************************************************
  */

#pragma GCC optimize ("O2")

#include "app.h"
#include "camera.h"
#include "gfx.h"
#include "lcd.h"
#include "main.h"
#include "ov5640.h"
#include "power.h"
#include "sd.h"
#include "settings.h"
#include "touch.h"
#include <stdio.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* Constants                                                                  */
/* -------------------------------------------------------------------------- */

#define COLOUR_YELLOW    0xFFE0U
#define COLOUR_GREY      0x8C71U
#define COLOUR_ACCENT    GFX_RGB(255, 150, 30)
#define COLOUR_ON        GFX_RGB(70, 220, 120)

#define TOUCH_POLL_MS    10U
#define TIMER_MS         3000U
#define FLASH_MS         90U
#define TOAST_MS         1200U
#define SAVE_DELAY_MS    3000U

#define STATUS_H         20
#define ICON_STEP        20

#define BAR_Y            200
#define BTN_Y            204
#define BTN_W            58
#define BTN_H            32
#define MENU_X           6
#define ZOOM_X           (GFX_W - 6 - BTN_W)
#define SHUT_CX          (GFX_W / 2)
#define SHUT_CY          219
#define SHUT_R           18

#define TAB_Y            23
#define TAB_W            86
#define TAB_H            28
#define TAB_COUNT        3
#define CLOSE_X          (6 + (TAB_COUNT * (TAB_W + 2)))
#define CLOSE_W          (GFX_W - 6 - CLOSE_X)

#define TILE_X           6
#define TILE_Y           55
#define TILE_W           100
#define TILE_H           68
#define TILE_GAP         4
#define TILES            6

typedef enum
{
  MI_STYLE, MI_WB,   MI_EFFECT, MI_EV_DOWN, MI_EV_UP, MI_NIGHT,
  MI_GRID,  MI_HIST, MI_ZEBRA,  MI_PEAK,    MI_TIMER, MI_ORIENT,
  MI_FPS,   MI_CAL,  MI_RESET,  MI_BATTERY, MI_CHIP,  MI_SD,
  MI_COUNT
} Menu_Item_t;

static const char *const tab_names[TAB_COUNT] =
  { "PICTURE", "TOOLS", "SYSTEM" };

static const char *const item_names[MI_COUNT] =
{
  "STYLE", "WHITE BAL", "EFFECT", "EXPOSURE -", "EXPOSURE +", "NIGHT",
  "GRID",  "HISTOGRAM", "ZEBRA",  "FOCUS PEAK", "SELF TIMER", "ROTATE",
  "FPS",   "TOUCH CAL", "RESET",  "POWER",      "CHIP TEMP",  "SD CARD",
};

static const char *const ev_names[7] =
  { "-1.0", "-0.7", "-0.3", "0.0", "+0.3", "+0.7", "+1.0" };
static const char *const style_names[OV5640_STYLE_COUNT] =
  { "STANDARD", "VIVID", "NATURAL" };
static const char *const wb_names[OV5640_WB_COUNT] =
  { "AUTO", "SUNNY", "CLOUDY", "OFFICE", "HOME" };
static const char *const effect_names[OV5640_EFFECT_COUNT] =
  { "NONE", "MONO", "SEPIA", "NEGATIVE", "RED", "GREEN", "BLUE" };
static const char *const orient_names[4] =
  { "NORMAL", "MIRROR", "FLIP", "180" };
static const char *const zoom_names[OV5640_ZOOM_COUNT] = { "1X", "2X", "4X" };

static const Icon_t wb_icons[OV5640_WB_COUNT] =
  { ICON_AUTO_WB, ICON_SUN, ICON_CLOUD, ICON_TUBE, ICON_BULB };

/* -------------------------------------------------------------------------- */
/* State                                                                      */
/* -------------------------------------------------------------------------- */

static Settings_t    app;
static bool          save_pending;
static uint32_t      save_at;

static bool          menu_open;
static int           menu_tab;
static bool          ui_hidden;
static bool          frozen;            /* reviewing a captured frame     */
static bool          freeze_shown;      /* ...and it is already on screen */
static uint32_t      flash_until;       /* white flash after the shutter  */
static uint32_t      timer_deadline;    /* self-timer running if non-zero */
static bool          cal_request;
static uint32_t      fps_display;
static uint32_t      shots;

static char          toast[24];
static uint32_t      toast_until;

static Touch_Point_t touch_now;
static int           touch_x;           /* last valid point of this press */
static int           touch_y;

/* -------------------------------------------------------------------------- */
/* Settings                                                                   */
/* -------------------------------------------------------------------------- */

static void app_defaults(void)
{
  memset(&app, 0, sizeof(app));
  app.style = (uint8_t)OV5640_STYLE_STANDARD;
}

/** Anything out of range (older firmware, bad flash) falls back to defaults. */
static bool app_valid(void)
{
  return (app.style  < OV5640_STYLE_COUNT)  &&
         (app.effect < OV5640_EFFECT_COUNT) &&
         (app.wb     < OV5640_WB_COUNT)     &&
         (app.ev    >= OV5640_EV_MIN)       &&
         (app.ev    <= OV5640_EV_MAX)       &&
         (app.orient < 4U)                  &&
         (app.zoom   < OV5640_ZOOM_COUNT);
}

/* The sensor table starts mirrored (0x3821 = 0x07), which is "normal" here. */
static void app_apply_orient(void)
{
  static const bool mirror[4] = { true, false, true,  false };
  static const bool flip[4]   = { false, false, true, true  };

  (void)OV5640_SetMirrorFlip(mirror[app.orient], flip[app.orient]);
}

void App_ApplySettings(void)
{
  (void)OV5640_SetEffect((OV5640_Effect_t)app.effect);
  (void)OV5640_SetStyle((OV5640_Style_t)app.style);
  (void)OV5640_SetWhiteBalance((OV5640_WhiteBalance_t)app.wb);
  (void)OV5640_SetExposureComp(app.ev);
  (void)OV5640_SetNightMode(app.night != 0U);
  (void)OV5640_SetZoom(app.zoom);
  app_apply_orient();
}

static void app_changed(void)
{
  save_pending = true;
  save_at      = HAL_GetTick() + SAVE_DELAY_MS;
}

static void app_toast(const char *name, const char *value)
{
  (void)snprintf(toast, sizeof(toast), "%s %s", name, value);
  toast_until = HAL_GetTick() + TOAST_MS;
}

/* -------------------------------------------------------------------------- */
/* Layout                                                                     */
/* -------------------------------------------------------------------------- */

static bool in_rect(int x, int y, int rx, int ry, int rw, int rh)
{
  return (x >= rx) && (x < (rx + rw)) && (y >= ry) && (y < (ry + rh));
}

static void tile_rect(int i, int *x, int *y)
{
  *x = TILE_X + ((i % 3) * (TILE_W + TILE_GAP));
  *y = TILE_Y + ((i / 3) * (TILE_H + TILE_GAP));
}

/* -------------------------------------------------------------------------- */
/* Actions                                                                    */
/* -------------------------------------------------------------------------- */

static void app_shoot(void)
{
  /* Flash first, then freeze on the next frame and hold it for review. */
  shots++;
  frozen       = true;
  freeze_shown = false;
  flash_until  = HAL_GetTick() + FLASH_MS;
  menu_open    = false;

  Lcd_FillAsync(LCD_WHITE);
}

static void app_shutter(void)
{
  if (frozen)
  {
    frozen = false;
  }
  else if (timer_deadline != 0U)
  {
    timer_deadline = 0U;            /* second press cancels the countdown */
  }
  else if (app.timer != 0U)
  {
    timer_deadline = HAL_GetTick() + TIMER_MS;
    menu_open      = false;
  }
  else
  {
    app_shoot();
  }
}

static void app_zoom(void)
{
  app.zoom = (uint8_t)((app.zoom + 1U) % OV5640_ZOOM_COUNT);
  (void)OV5640_SetZoom(app.zoom);
  app_toast("ZOOM", zoom_names[app.zoom]);
  app_changed();
}

static void menu_select(int item)
{
  switch (item)
  {
    case MI_STYLE:
      app.style = (uint8_t)((app.style + 1U) % OV5640_STYLE_COUNT);
      (void)OV5640_SetStyle((OV5640_Style_t)app.style);
      break;

    case MI_WB:
      app.wb = (uint8_t)((app.wb + 1U) % OV5640_WB_COUNT);
      (void)OV5640_SetWhiteBalance((OV5640_WhiteBalance_t)app.wb);
      break;

    case MI_EFFECT:
      app.effect = (uint8_t)((app.effect + 1U) % OV5640_EFFECT_COUNT);
      (void)OV5640_SetEffect((OV5640_Effect_t)app.effect);
      break;

    case MI_EV_DOWN:
      if (app.ev > OV5640_EV_MIN) { app.ev--; }
      (void)OV5640_SetExposureComp(app.ev);
      break;

    case MI_EV_UP:
      if (app.ev < OV5640_EV_MAX) { app.ev++; }
      (void)OV5640_SetExposureComp(app.ev);
      break;

    case MI_NIGHT:
      app.night = !app.night;
      (void)OV5640_SetNightMode(app.night != 0U);
      break;

    case MI_GRID:   app.grid  = !app.grid;  break;
    case MI_HIST:   app.hist  = !app.hist;  break;
    case MI_ZEBRA:  app.zebra = !app.zebra; break;
    case MI_PEAK:   app.peak  = !app.peak;  break;
    case MI_TIMER:  app.timer = !app.timer; break;
    case MI_FPS:    app.fps   = !app.fps;   break;

    case MI_ORIENT:
      app.orient = (uint8_t)((app.orient + 1U) % 4U);
      app_apply_orient();
      break;

    case MI_CAL:
      cal_request = true;
      menu_open   = false;
      return;

    case MI_RESET:
      app_defaults();
      App_ApplySettings();
      menu_open = false;
      app_toast("SETTINGS", "RESET");
      break;

    default:                          /* information tiles */
      return;
  }

  app_changed();
}

/** A finger was lifted; (x, y) is where it last was. */
static void app_tap(int x, int y)
{
  if (frozen)
  {
    frozen = false;
    return;
  }

  if (ui_hidden)
  {
    ui_hidden = false;
    return;
  }

  if (y >= BAR_Y)
  {
    if (x < (MENU_X + BTN_W + 10))
    {
      menu_open = !menu_open;
    }
    else if (x >= (ZOOM_X - 10))
    {
      app_zoom();
    }
    else if (in_rect(x, y, SHUT_CX - 30, BAR_Y, 60, GFX_H - BAR_Y))
    {
      app_shutter();
    }

    return;
  }

  if (!menu_open)
  {
    ui_hidden = true;
    return;
  }

  if (y < TILE_Y)
  {
    int tab = (x - 6) / (TAB_W + 2);

    if (x >= CLOSE_X)
    {
      menu_open = false;
    }
    else if ((tab >= 0) && (tab < TAB_COUNT))
    {
      menu_tab = tab;
    }

    return;
  }

  for (int i = 0; i < TILES; i++)
  {
    int tx;
    int ty;

    tile_rect(i, &tx, &ty);

    if (in_rect(x, y, tx, ty, TILE_W, TILE_H))
    {
      menu_select((menu_tab * TILES) + i);
      return;
    }
  }
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

void App_Init(void)
{
  if (!Settings_Load(&app) || !app_valid())
  {
    app_defaults();
  }

  App_ApplySettings();
  Power_Init();
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

  /* Wait until the user is done changing things, to spare the flash. */
  if (save_pending && ((int32_t)(now - save_at) >= 0))
  {
    save_pending = false;
    Settings_Save(&app);
  }

  Power_Poll();

  if (Lcd_IsBusy())
  {
    return;
  }

  /* Shares SPI1 with the LCD and touch, so only while the LCD is idle. */
  Sd_Poll();

  if ((now - last_poll) < TOUCH_POLL_MS)
  {
    return;
  }

  last_poll = now;

  Touch_Read(&touch_now);

  if (touch_now.pressed)
  {
    touch_x = touch_now.x;
    touch_y = touch_now.y;
  }
  else if (was_pressed)
  {
    app_tap(touch_x, touch_y);
  }

  was_pressed = touch_now.pressed;
}

bool App_WantsFrame(void)
{
  if ((int32_t)(HAL_GetTick() - flash_until) < 0)
  {
    return false;                     /* white flash still on screen */
  }

  return !(frozen && freeze_shown);
}

/* -------------------------------------------------------------------------- */
/* Drawing                                                                    */
/* -------------------------------------------------------------------------- */

static void text_centred(uint16_t *fb, int x, int w, int y, const char *s,
                         uint16_t colour, int scale)
{
  (void)Gfx_Text(fb, x + ((w - Gfx_TextWidth(s, scale)) / 2), y, s, colour,
                 scale);
}

static uint16_t battery_colour(int percent)
{
  if (percent < POWER_LOW_PERCENT) { return LCD_RED; }
  if (percent < 50)                { return COLOUR_YELLOW; }
  return COLOUR_ON;
}

static void draw_status(uint16_t *fb)
{
  bool blink = ((HAL_GetTick() / 400U) & 1U) != 0U;
  int  x     = 6;
  int  rx;
  char text[16];

  Gfx_Darken(fb, 0, 0, GFX_W, STATUS_H);
  Gfx_Darken(fb, 0, 0, GFX_W, STATUS_H);

  /* Left: what the picture is set to. */
  Icon_Draw(fb, x, 2, wb_icons[app.wb], LCD_WHITE, 1);
  x += ICON_STEP;

  x += Gfx_Text(fb, x, 7, style_names[app.style], COLOUR_ACCENT, 1) + 4;

  if (app.effect != OV5640_EFFECT_NORMAL)
  {
    Icon_EffectSwatch(fb, x, 2, (int)app.effect, 1);
    x += ICON_STEP;
  }

  if (app.ev != 0)
  {
    x += Gfx_Text(fb, x, 7, ev_names[app.ev - OV5640_EV_MIN], LCD_WHITE, 1) + 4;
  }

  if (app.night)
  {
    Icon_Draw(fb, x, 2, ICON_MOON, COLOUR_YELLOW, 1);
    x += ICON_STEP;
  }

  if (app.timer)
  {
    Icon_Draw(fb, x, 2, ICON_TIMER, COLOUR_ACCENT, 1);
  }

  /* Right, from the edge inwards: power, storage, temperature, frame rate. */
  rx = GFX_W - 6;

  if (Power_HasBattery())
  {
    int percent = Power_GetPercent();

    (void)snprintf(text, sizeof(text), "%d%%", percent);
    rx -= Gfx_TextWidth(text, 1);
    (void)Gfx_Text(fb, rx, 7, text, LCD_WHITE, 1);
    rx -= 4 + 23;

    if ((percent >= POWER_CRITICAL_PERCENT) || blink)
    {
      Icon_Battery(fb, rx, 4, percent, battery_colour(percent), 1);
    }
  }
  else
  {
    rx -= ICON_SIZE;
    Icon_Draw(fb, rx, 2, ICON_PLUG, LCD_WHITE, 1);
  }

  if (Sd_IsPresent())
  {
    rx -= ICON_STEP;
    Icon_Draw(fb, rx, 2, ICON_SD, LCD_WHITE, 1);
  }

  if (Power_GetTempC() >= POWER_HOT_CELSIUS)
  {
    rx -= ICON_STEP;
    Icon_Draw(fb, rx, 2, ICON_THERMO, blink ? LCD_RED : COLOUR_ACCENT, 1);
  }

  if (app.fps)
  {
    (void)snprintf(text, sizeof(text), "%luFPS", (unsigned long)fps_display);
    rx -= 8 + Gfx_TextWidth(text, 1);
    (void)Gfx_Text(fb, rx, 7, text, COLOUR_GREY, 1);
  }
}

static void draw_bar(uint16_t *fb)
{
  Gfx_Panel(fb, MENU_X, BTN_Y, BTN_W, BTN_H);
  Icon_Draw(fb, MENU_X + ((BTN_W - 16) / 2), BTN_Y + 8,
            menu_open ? ICON_CLOSE : ICON_MENU, LCD_WHITE, 1);

  Gfx_Panel(fb, ZOOM_X, BTN_Y, BTN_W, BTN_H);
  text_centred(fb, ZOOM_X, BTN_W, BTN_Y + 9, zoom_names[app.zoom],
               (app.zoom != 0U) ? COLOUR_ACCENT : LCD_WHITE, 2);

  /* Round shutter: white ring, dark gap, coloured centre. */
  Gfx_Disc(fb, SHUT_CX, SHUT_CY, SHUT_R, LCD_WHITE);
  Gfx_Disc(fb, SHUT_CX, SHUT_CY, SHUT_R - 2, 0x0000U);
  Gfx_Disc(fb, SHUT_CX, SHUT_CY, SHUT_R - 4,
           (timer_deadline != 0U) ? LCD_RED :
           ((app.timer != 0U) ? COLOUR_ACCENT : LCD_WHITE));

  if (app.timer)
  {
    Icon_Draw(fb, SHUT_CX - 8, SHUT_CY - 8, ICON_TIMER, 0x0000U, 1);
  }
}

static bool item_on(int item)
{
  switch (item)
  {
    case MI_NIGHT: return app.night != 0U;
    case MI_GRID:  return app.grid  != 0U;
    case MI_HIST:  return app.hist  != 0U;
    case MI_ZEBRA: return app.zebra != 0U;
    case MI_PEAK:  return app.peak  != 0U;
    case MI_TIMER: return app.timer != 0U;
    case MI_FPS:   return app.fps   != 0U;
    default:       return false;
  }
}

static Icon_t item_icon(int item)
{
  switch (item)
  {
    case MI_STYLE:   return ICON_CAMERA;
    case MI_WB:      return wb_icons[app.wb];
    case MI_EV_DOWN: return ICON_MINUS;
    case MI_EV_UP:   return ICON_PLUS;
    case MI_NIGHT:   return ICON_MOON;
    case MI_GRID:    return ICON_GRID;
    case MI_HIST:    return ICON_HISTOGRAM;
    case MI_ZEBRA:   return ICON_ZEBRA;
    case MI_PEAK:    return ICON_PEAKING;
    case MI_TIMER:   return ICON_TIMER;
    case MI_ORIENT:  return ICON_ORIENT;
    case MI_FPS:     return ICON_ZOOM;
    case MI_CAL:     return ICON_TARGET;
    case MI_RESET:   return ICON_CLOSE;
    case MI_BATTERY: return ICON_PLUG;
    case MI_CHIP:    return ICON_THERMO;
    case MI_SD:
    default:         return ICON_SD;
  }
}

static const char *item_value(int item, char *buf, size_t n)
{
  switch (item)
  {
    case MI_STYLE:   return style_names[app.style];
    case MI_WB:      return wb_names[app.wb];
    case MI_EFFECT:  return effect_names[app.effect];
    case MI_EV_DOWN:
    case MI_EV_UP:   return ev_names[app.ev - OV5640_EV_MIN];
    case MI_ORIENT:  return orient_names[app.orient];
    case MI_CAL:
    case MI_RESET:   return "START";
    case MI_SD:      return Sd_IsPresent() ? "PRESENT" : "NONE";

    case MI_BATTERY:
      if (!Power_HasBattery()) { return "EXTERNAL"; }
      (void)snprintf(buf, n, "%d%% %lu.%02luV", Power_GetPercent(),
                     (unsigned long)(Power_GetBatteryMv() / 1000U),
                     (unsigned long)((Power_GetBatteryMv() % 1000U) / 10U));
      return buf;

    case MI_CHIP:
      (void)snprintf(buf, n, "%d C", Power_GetTempC());
      return buf;

    default:
      return item_on(item) ? "ON" : "OFF";
  }
}

static void draw_menu(uint16_t *fb)
{
  char buf[32];

  for (int t = 0; t < TAB_COUNT; t++)
  {
    int x = 6 + (t * (TAB_W + 2));

    if (t == menu_tab)
    {
      Gfx_FillRound(fb, x, TAB_Y, TAB_W, TAB_H, COLOUR_ACCENT);
    }
    else
    {
      Gfx_Panel(fb, x, TAB_Y, TAB_W, TAB_H);
    }

    text_centred(fb, x, TAB_W, TAB_Y + 11, tab_names[t],
                 (t == menu_tab) ? 0x0000U : LCD_WHITE, 1);
  }

  Gfx_Panel(fb, CLOSE_X, TAB_Y, CLOSE_W, TAB_H);
  Icon_Draw(fb, CLOSE_X + ((CLOSE_W - 16) / 2), TAB_Y + 6, ICON_CLOSE,
            LCD_WHITE, 1);

  for (int i = 0; i < TILES; i++)
  {
    int      item = (menu_tab * TILES) + i;
    bool     on   = item_on(item);
    uint16_t c    = on ? COLOUR_ON : LCD_WHITE;
    int      x;
    int      y;

    tile_rect(i, &x, &y);

    Gfx_Panel(fb, x, y, TILE_W, TILE_H);

    if (on)
    {
      Gfx_Fill(fb, x + 8, y + TILE_H - 3, TILE_W - 16, 2, COLOUR_ON);
    }

    if (item == MI_EFFECT)
    {
      Icon_EffectSwatch(fb, x + ((TILE_W - 32) / 2), y + 5, (int)app.effect, 2);
    }
    else
    {
      Icon_Draw(fb, x + ((TILE_W - 32) / 2), y + 5, item_icon(item), c, 2);
    }

    text_centred(fb, x, TILE_W, y + 42, item_names[item], LCD_WHITE, 1);
    text_centred(fb, x, TILE_W, y + 54, item_value(item, buf, sizeof(buf)),
                 on ? COLOUR_ON : COLOUR_ACCENT, 1);
  }
}

/** Large blinking battery in the middle of the screen when nearly empty. */
static void draw_battery_warning(uint16_t *fb)
{
  if (!Power_HasBattery() ||
      (Power_GetPercent() >= POWER_CRITICAL_PERCENT) ||
      (((HAL_GetTick() / 400U) & 1U) == 0U))
  {
    return;
  }

  Gfx_Panel(fb, (GFX_W / 2) - 50, (GFX_H / 2) - 26, 100, 52);
  Icon_Battery(fb, (GFX_W / 2) - 35, (GFX_H / 2) - 18, 0, LCD_RED, 3);
}

void App_ProcessFrame(uint16_t *frame)
{
  static uint32_t bins[64];
  uint32_t        now = HAL_GetTick();

  if (frozen)
  {
    char count[16];

    /* Review: the clean picture with a small caption. */
    Gfx_Frame(frame, 0, 0, GFX_W, GFX_H, LCD_WHITE);
    Gfx_Frame(frame, 1, 1, GFX_W - 2, GFX_H - 2, LCD_WHITE);
    Gfx_Panel(frame, 8, 8, 76, 24);
    Icon_Draw(frame, 14, 12, ICON_CAMERA, LCD_WHITE, 1);
    (void)snprintf(count, sizeof(count), "%lu", (unsigned long)shots);
    (void)Gfx_Text(frame, 38, 17, count, COLOUR_ACCENT, 1);
    Gfx_Panel(frame, 90, 212, 140, 20);
    text_centred(frame, 90, 140, 218, "TAP TO CONTINUE", LCD_WHITE, 1);
    freeze_shown = true;
    return;
  }

  /* Image tools first, on the picture itself... */
  if (app.hist)  { Gfx_Histogram(frame, bins); }
  if (app.peak)  { Gfx_FocusPeaking(frame, 14U); }
  if (app.zebra) { Gfx_Zebra(frame, 60U, now / 60U); }
  if (app.grid)  { Gfx_Grid(frame); }

  /* ...then the interface on top. */
  if (!ui_hidden)
  {
    draw_status(frame);

    if (menu_open)
    {
      draw_menu(frame);
    }
    else if (app.hist)
    {
      Gfx_DrawHistogram(frame, 10, 154, 36, bins);
    }

    draw_bar(frame);
  }

  if (!menu_open && ((int32_t)(toast_until - now) > 0))
  {
    int w = Gfx_TextWidth(toast, 2) + 20;

    Gfx_Panel(frame, (GFX_W - w) / 2, 30, w, 26);
    text_centred(frame, 0, GFX_W, 36, toast, LCD_WHITE, 2);
  }

  if (timer_deadline != 0U)
  {
    char     digit[2];
    uint32_t left = (timer_deadline - now + 999U) / 1000U;

    digit[0] = (char)('0' + (left > 9U ? 9U : left));
    digit[1] = '\0';
    text_centred(frame, 0, GFX_W, (GFX_H - 56) / 2, digit, COLOUR_ACCENT, 8);
  }

  draw_battery_warning(frame);
}

void App_ShowFault(Icon_t icon)
{
  uint16_t *fb   = (uint16_t *)Camera_GetFrameBuffer(0U);
  int       size = ICON_SIZE * 6;

  Lcd_WaitIdle();
  Gfx_Fill(fb, 0, 0, GFX_W, GFX_H, 0x0000U);
  Icon_Draw(fb, (GFX_W - size) / 2, 56, icon, LCD_WHITE, 6);
  Icon_Slash(fb, (GFX_W - size) / 2, 56, 6);
  text_centred(fb, 0, GFX_W, 176, "CAMERA NOT FOUND", LCD_WHITE, 2);
  text_centred(fb, 0, GFX_W, 204, "CHECK CAMERA WIRING", COLOUR_GREY, 1);
  Lcd_BlitAsync(fb);
  Lcd_WaitIdle();
}

void App_SetFps(uint32_t display_fps)
{
  fps_display = display_fps;
}

bool App_TakeCalibrationRequest(void)
{
  bool request = cal_request;

  cal_request = false;

  return request;
}
