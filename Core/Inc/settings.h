/**
  ******************************************************************************
  * @file    settings.h
  * @brief   User settings, kept in flash across power cycles.
  ******************************************************************************
  */

#ifndef __SETTINGS_H
#define __SETTINGS_H

#include <stdint.h>
#include <stdbool.h>

typedef struct
{
  uint8_t style;
  uint8_t effect;
  uint8_t wb;
  int8_t  ev;
  uint8_t orient;
  uint8_t zoom;
  uint8_t night;
  uint8_t grid;
  uint8_t hist;
  uint8_t zebra;
  uint8_t peak;
  uint8_t timer;
  uint8_t fps;
  uint8_t spare[3];
} Settings_t;

/** Load the newest saved settings. Returns false if there are none. */
bool Settings_Load(Settings_t *s);

/** Append to the log in flash; does nothing if unchanged. */
void Settings_Save(const Settings_t *s);

#endif /* __SETTINGS_H */
