/**
  ******************************************************************************
  * @file    power.h
  * @brief   Battery gauge and die temperature on ADC1.
  *
  * Battery: a 1-cell Li-ion/LiPo measured through a 100k/100k divider into
  * PA3 (Arduino A0, ADC1_IN3), with 100 nF from PA3 to GND.  The divider
  * halves 4.2 V to 2.1 V, inside the 3.3 V ADC range.  Readings are scaled
  * by the internal reference, so they do not depend on the exact 3V3 rail.
  *
  * With nothing on PA3 the gauge reports "no battery" rather than noise:
  * the pin is discharged just before each conversion, so a floating input
  * reads ~0 V while a real divider recharges it within microseconds.
  ******************************************************************************
  */

#ifndef __POWER_H
#define __POWER_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/* Thresholds for the UI. */
#define POWER_LOW_PERCENT        15
#define POWER_CRITICAL_PERCENT   5
#define POWER_HOT_CELSIUS        75

void Power_Init(void);

/** Take fresh readings at most every 500 ms. Cheap to call every loop. */
void Power_Poll(void);

/** False when no battery is connected (running from external 5 V). */
bool Power_HasBattery(void);

/** Estimated charge, 0..100. Meaningless without a battery. */
int Power_GetPercent(void);

/** Smoothed battery voltage in millivolts. */
uint32_t Power_GetBatteryMv(void);

/** MCU die temperature, degrees Celsius. */
int Power_GetTempC(void);

#ifdef __cplusplus
}
#endif

#endif /* __POWER_H */
