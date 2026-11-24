/**
  ******************************************************************************
  * @file    sd.h
  * @brief   SD card in SPI mode on the bus shared with the LCD and touch.
  *
  * Every command and data block carries a CRC that is checked at the far end,
  * so a bad wire gives an error instead of bad data.  Writing is refused
  * unless Sd_AllowWrite(true) is in force, and never touches the partition
  * table or the volume's boot sector.
  ******************************************************************************
  */

#ifndef __SD_H
#define __SD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/** Detect insertion and removal, at most every 2 s. LCD must be idle. */
void Sd_Poll(void);

/** True while an initialised card is answering. */
bool Sd_IsPresent(void);

/** Open or close the write gate. */
void Sd_AllowWrite(bool allow);

/** Never write this sector (the volume's boot sector). */
void Sd_Protect(uint32_t sector);

#ifdef __cplusplus
}
#endif

#endif /* __SD_H */
