/**
  ******************************************************************************
  * @file    photo.h
  * @brief   Photos on the SD card: save, list and show.
  *
  * Existing files are only ever read.  Saving creates a new file under the
  * next free number; nothing here deletes, renames or overwrites.
  ******************************************************************************
  */

#ifndef __PHOTO_H
#define __PHOTO_H

#include <stdint.h>
#include <stdbool.h>

/* 0 keeps the card read-only: the write path is not proven on hardware yet. */
#define PHOTO_SAVE_ENABLED  0

/**
  * @brief  Save a 320x240 RGB565 frame as OV5640/IMG_nnnn.BMP and read it
  *         back to check it.
  * @param  name  receives the file name, 16 bytes.
  * @retval NULL if saved and verified, else a short reason.
  */
const char *Photo_Save(const uint16_t *frame, char *name);

/** List the JPG and BMP files. Returns how many, or -1 without a card. */
int Photo_Scan(void);

/** Path of photo i from the last scan. */
const char *Photo_Path(int i);

/** Number of entries at the front of the list that this camera took. */
int Photo_OwnCount(void);

/**
  * @brief  Draw photo i, fitted to 320x240.
  * @retval NULL if shown, else a short reason.
  */
const char *Photo_Show(int i, uint16_t *fb);

#endif /* __PHOTO_H */
