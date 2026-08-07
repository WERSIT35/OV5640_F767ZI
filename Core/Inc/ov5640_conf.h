/**
  ******************************************************************************
  * @file    ov5640_conf.h
  * @brief   Board-level configuration for the OV5640 camera on this target.
  *
  * Everything that depends on how the module is wired, or on how fast the
  * MCU is clocked, lives here.  ov5640.c / camera.c contain no board details.
  ******************************************************************************
  */

#ifndef __OV5640_CONF_H
#define __OV5640_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

/* -------------------------------------------------------------------------- */
/* SCCB (I2C) bus                                                             */
/* -------------------------------------------------------------------------- */

/* The OV5640 SCCB slave address is 0x3C (7-bit).  HAL wants it pre-shifted. */
#define OV5640_I2C_ADDRESS        (0x3C << 1)
#define OV5640_I2C_HANDLE         hi2c2
#define OV5640_I2C_TIMEOUT_MS     100U

/* -------------------------------------------------------------------------- */
/* Control pins                                                               */
/* -------------------------------------------------------------------------- */

/*
 * Determined empirically by sweeping all four combinations and watching for
 * a chip-ID ACK: the sensor answers only at PE2 high, PD7 low.  Since RESET
 * is active low and PWDN is active high, that pins the mapping down as
 * below - the reverse of the initial assumption.
 */

/* PD7 -> PWDN  (active HIGH: sensor is powered down while this is high)      */
#define OV5640_PWDN_PORT          GPIOD
#define OV5640_PWDN_PIN           GPIO_PIN_7

/* PE2 -> RESET (active LOW: sensor is held in reset while this is low)       */
#define OV5640_RESET_PORT         GPIOE
#define OV5640_RESET_PIN          GPIO_PIN_2

/* -------------------------------------------------------------------------- */
/* Output format                                                              */
/* -------------------------------------------------------------------------- */

#define OV5640_FRAME_WIDTH        320U
#define OV5640_FRAME_HEIGHT       240U
#define OV5640_BYTES_PER_PIXEL    2U
#define OV5640_FRAME_SIZE_BYTES   (OV5640_FRAME_WIDTH * OV5640_FRAME_HEIGHT * \
                                   OV5640_BYTES_PER_PIXEL)

/*
 * RGB565 byte order out of the DVP port (register 0x4300).
 *
 * DCMI packs the first byte received into the LOW byte of each 16-bit word,
 * so for a conventional little-endian RGB565 pixel (RRRRRGGG GGGBBBBB in a
 * uint16) the sensor must emit {g[2:0],b[4:0]} first and {r[4:0],g[5:3]}
 * second.
 *
 *   0x6F -> RGB565, {b[4:0],g[5:3]} then {g[2:0],r[4:0]}
 *   0x61 -> the same with the two bytes swapped
 *
 * If your image comes out with red and blue exchanged, flip this to 0x61
 * (or pass --swap to the host-side receiver script).
 */
#define OV5640_REG_4300_RGB565    0x6FU

/* -------------------------------------------------------------------------- */
/* Pixel clock                                                                */
/* -------------------------------------------------------------------------- */

/*
 * The DCMI can only sample DCMI_PIXCLK if its period is longer than
 * 2.5 x HCLK.  This project currently runs on HSI with no PLL:
 *
 *     HCLK = 16 MHz  ->  PIXCLK must stay below ~6.4 MHz
 *
 * PCLK is scaled by the sensor PLL system divider in register 0x3035[7:4].
 * With the PLL settings in ov5640_regs.h (24 MHz XCLK, pre-div 3,
 * multiplier 105, root divider /2, 8-bit mode, 0x3824 = /2) the divider maps
 * roughly as:
 *
 *     sysdiv  4  ->  ~13   MHz    (needs HCLK >= 33 MHz)
 *     sysdiv  6  ->  ~8.7  MHz    (needs HCLK >= 22 MHz)
 *     sysdiv  8  ->  ~6.5  MHz    (needs HCLK >= 17 MHz)
 *     sysdiv 12  ->  ~4.4  MHz    (safe at 16 MHz)   <-- default
 *     sysdiv 16  ->  ~3.3  MHz
 *
 * Frame rate is roughly PCLK / 1.87e6 for the VGA line timing used here,
 * so sysdiv 12 gives about 2 fps.  Raise HCLK (enable the PLL in CubeMX)
 * and then lower this number to go faster.  If you set it too low, the
 * DCMI overrun flag fires and Camera_Capture() reports CAMERA_ERR_OVERRUN.
 */
#define OV5640_PLL_SYSDIV         15U

/* -------------------------------------------------------------------------- */
/* XCLK (sensor master clock)                                                 */
/* -------------------------------------------------------------------------- */

/*
 * The OV5640 does nothing at all without a clock on XCLK - including
 * acknowledge its own SCCB address, because the serial interface block is
 * clocked from it.  A camera that NACKs every I2C address is the classic
 * symptom of a missing XCLK.
 *
 * Many modules carry their own 24 MHz oscillator.  If yours does not, set
 * this to 1 and wire PA8 (MCO1) to the module's XCLK / MCLK pin.
 *
 * With the PLL disabled, MCO1 can only pass HSI straight through at 16 MHz.
 * That sits inside the OV5640's 6-27 MHz XCLK range, and it scales every
 * PCLK estimate in this file by 16/24 - making them more conservative, not
 * less.  Leaving this enabled is harmless if you never wire PA8 up.
 */
#define OV5640_DRIVE_XCLK_FROM_MCO1   1

/* -------------------------------------------------------------------------- */
/* Image quality / diagnostics                                                */
/* -------------------------------------------------------------------------- */

/*
 * Set to 1 to replace the live image with the sensor's internal colour-bar
 * pattern.  This is the single most useful test for separating problems:
 *
 *   clean, sharp bars  -> DCMI, DMA, wiring and byte order are all correct,
 *                         and any noise in the real image is coming from the
 *                         sensor itself (gain, exposure, lighting, optics)
 *   noisy or torn bars -> the fault is in the capture path: sampling edge,
 *                         pixel clock, or signal integrity on the data lines
 *
 * The bars are generated after the ISP, so they exercise everything from the
 * DVP pins onwards.
 */
#define OV5640_ENABLE_TEST_PATTERN  0

/*
 * Ceiling on AEC gain, register 0x3A18/0x3A19.  The default (0x00F8 = 31x)
 * lets the sensor amplify a dark scene until noise swamps it.  Lowering this
 * trades a darker image for a much cleaner one - worth doing if the scene is
 * dim and you care more about clarity than brightness.
 *
 *   0x00F8 -> 31x (default, noisiest)
 *   0x0078 -> 15x
 *   0x0038 ->  7x (cleanest, needs good lighting)
 */
#define OV5640_GAIN_CEILING         0x00F8U

/* -------------------------------------------------------------------------- */
/* Power-up timing (milliseconds)                                             */
/* -------------------------------------------------------------------------- */

#define OV5640_T_PWDN_MS          5U    /* PWDN settle before releasing reset */
#define OV5640_T_RESET_MS         20U   /* reset release -> SCCB ready        */
#define OV5640_T_SWRESET_MS       10U   /* software reset (0x3008 = 0x82)     */
#define OV5640_T_STREAM_MS        300U  /* wake-up -> AEC/AWB settled         */

#ifdef __cplusplus
}
#endif

#endif /* __OV5640_CONF_H */
