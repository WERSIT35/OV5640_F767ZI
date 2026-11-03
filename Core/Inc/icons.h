/**
  ******************************************************************************
  * @file    icons.h
  * @brief   16x16 one-colour icons for the camera UI, plus a few drawn shapes
  *          (battery, colour swatch) that carry live values.
  ******************************************************************************
  */

#ifndef __ICONS_H
#define __ICONS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

#define ICON_SIZE   16

typedef enum
{
  ICON_SUN,
  ICON_CLOUD,
  ICON_BULB,
  ICON_TUBE,
  ICON_AUTO_WB,
  ICON_MOON,
  ICON_TIMER,
  ICON_ZOOM,
  ICON_GRID,
  ICON_HISTOGRAM,
  ICON_ZEBRA,
  ICON_PEAKING,
  ICON_ORIENT,
  ICON_PENCIL,
  ICON_TARGET,
  ICON_CAMERA,
  ICON_CLOSE,
  ICON_MENU,
  ICON_MINUS,
  ICON_PLUS,
  ICON_SD,
  ICON_PLUG,
  ICON_THERMO,
  ICON_COUNT
} Icon_t;

/** Draw an icon with its top-left corner at (x, y), each pixel scale x scale. */
void Icon_Draw(uint16_t *fb, int x, int y, Icon_t icon, uint16_t colour,
               int scale);

/**
  * @brief  Battery outline with a level bar, 24 x 12 at scale 1.
  * @param  percent  0..100.
  */
void Icon_Battery(uint16_t *fb, int x, int y, int percent, uint16_t colour,
                  int scale);

/** Red diagonal slash across an icon-sized square: "missing / off". */
void Icon_Slash(uint16_t *fb, int x, int y, int scale);

/** Four-colour swatch previewing a colour effect (index = OV5640_Effect_t). */
void Icon_EffectSwatch(uint16_t *fb, int x, int y, int effect, int scale);

#ifdef __cplusplus
}
#endif

#endif /* __ICONS_H */
