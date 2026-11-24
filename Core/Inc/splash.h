/**
  ******************************************************************************
  * @file    splash.h
  * @brief   Start-up screen: opening aperture, title and progress bar.
  ******************************************************************************
  */

#ifndef __SPLASH_H
#define __SPLASH_H

/** Opening animation, about a second. The LCD must be initialised. */
void Splash_Intro(void);

/** Show a loading step; percent drives the bar. */
void Splash_Step(const char *text, int percent);

#endif /* __SPLASH_H */
