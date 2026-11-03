/**
  ******************************************************************************
  * @file    sd.h
  * @brief   SD card presence check on the TFT module's slot, sharing SPI1.
  *
  * Wiring: SD_SCK -> PB3, SD_MOSI -> PB5, SD_MISO -> PB4, SD_CS -> PF15
  * (Arduino D2).  The slot has no card-detect switch brought out, so the
  * card is detected by asking it: a GO_IDLE_STATE (CMD0) that comes back
  * with R1 = 0x01 means a card is there and talking.
  *
  * Like the touch controller, call only while the LCD is idle.
  ******************************************************************************
  */

#ifndef __SD_H
#define __SD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>

/** Probe at most every 2 s; cheap to call every loop. LCD must be idle. */
void Sd_Poll(void);

/** Result of the last probe. */
bool Sd_IsPresent(void);

#ifdef __cplusplus
}
#endif

#endif /* __SD_H */
