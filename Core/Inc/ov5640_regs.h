/**
  ******************************************************************************
  * @file    ov5640_regs.h
  * @brief   OV5640 register definitions and initialisation tables.
  *
  * The main table follows the OmniVision "OV5640 Camera Module Software
  * Application Notes" reference sequence, trimmed to DVP (parallel) output
  * and re-ordered so that the PLL / pad-output writes are grouped and
  * commented.  The QVGA table on the end reprograms the scaler for
  * 320x240 RGB565.
  ******************************************************************************
  */

#ifndef __OV5640_REGS_H
#define __OV5640_REGS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include "ov5640_conf.h"

/* -------------------------------------------------------------------------- */
/* Named registers used by the driver                                         */
/* -------------------------------------------------------------------------- */

#define OV5640_REG_CHIP_ID_HIGH      0x300AU
#define OV5640_REG_CHIP_ID_LOW       0x300BU
#define OV5640_CHIP_ID               0x5640U

#define OV5640_REG_SYSTEM_CTRL0      0x3008U   /* [7] sw reset, [6] sw powerdn */
#define OV5640_REG_SC_PLL_CTRL0      0x3034U
#define OV5640_REG_SC_PLL_CTRL1      0x3035U   /* [7:4] system clock divider   */
#define OV5640_REG_SC_PLL_CTRL2      0x3036U   /* PLL multiplier               */
#define OV5640_REG_SC_PLL_CTRL3      0x3037U
#define OV5640_REG_FORMAT_CTRL00     0x4300U
#define OV5640_REG_ISP_FORMAT_MUX    0x501FU
#define OV5640_REG_TIMING_TC_REG20   0x3820U   /* vertical flip                */
#define OV5640_REG_TIMING_TC_REG21   0x3821U   /* horizontal mirror            */
#define OV5640_REG_TEST_PATTERN      0x503DU

#define OV5640_SYSTEM_CTRL0_SWRESET  0x82U
#define OV5640_SYSTEM_CTRL0_PWDN     0x42U
#define OV5640_SYSTEM_CTRL0_STREAM   0x02U

/* Marker used inside the tables below to request a delay instead of a write. */
#define OV5640_REG_DELAY             0xFFFFU

typedef struct
{
  uint16_t reg;
  uint8_t  val;
} OV5640_RegVal_t;

/* -------------------------------------------------------------------------- */
/* Common initialisation                                                      */
/* -------------------------------------------------------------------------- */

static const OV5640_RegVal_t OV5640_Common[] =
{
  /* --- reset ------------------------------------------------------------ */
  { 0x3103, 0x11 },                       /* SCCB clock from pad             */
  { 0x3008, 0x82 },                       /* software reset                  */
  { OV5640_REG_DELAY, 10 },
  { 0x3008, 0x42 },                       /* software power down             */
  { 0x3103, 0x03 },                       /* SCCB clock from PLL             */

  /* --- pad output control ---------------------------------------------- */
  /* Both PAD_OUTPUT_ENABLE registers must be driven for DVP output.  0xFF
   * enables every pad; the ones this board does not use (FREX, D[1:0],
   * GPIO[1:0]) are left unconnected on the module.  If your module ties any
   * of those to the host, narrow this to 0x7F / 0xFC.                      */
  { 0x3017, 0xFF },
  { 0x3018, 0xFF },

  /* --- PLL -------------------------------------------------------------- */
  /* 0x3035 is patched at runtime with OV5640_PLL_SYSDIV, see ov5640.c.     */
  { 0x3034, 0x18 },                       /* charge pump, 8-bit output mode  */
  { 0x3035, 0x11 },                       /* [7:4] sysdiv, [3:0] scale div   */
  { 0x3036, 0x69 },                       /* PLL multiplier = 105            */
  { 0x3037, 0x13 },                       /* root divider /2, pre-divider /3 */
  { 0x3108, 0x01 },                       /* PCLK/SCLK root dividers         */

  /* --- analog / sensor core (OmniVision recommended values) ------------- */
  { 0x3630, 0x36 },
  { 0x3631, 0x0E },
  { 0x3632, 0xE2 },
  { 0x3633, 0x12 },
  { 0x3621, 0xE0 },
  { 0x3704, 0xA0 },
  { 0x3703, 0x5A },
  { 0x3715, 0x78 },
  { 0x3717, 0x01 },
  { 0x370B, 0x60 },
  { 0x3705, 0x1A },
  { 0x3905, 0x02 },
  { 0x3906, 0x10 },
  { 0x3901, 0x0A },
  { 0x3731, 0x12 },
  { 0x3600, 0x08 },                       /* VCM control                     */
  { 0x3601, 0x33 },                       /* VCM control                     */
  { 0x302D, 0x60 },
  { 0x3620, 0x52 },
  { 0x371B, 0x20 },
  { 0x471C, 0x50 },
  { 0x3635, 0x13 },
  { 0x3636, 0x03 },
  { 0x3634, 0x40 },
  { 0x3622, 0x01 },
  { 0x3618, 0x00 },
  { 0x3612, 0x29 },
  { 0x3708, 0x64 },
  { 0x3709, 0x52 },
  { 0x370C, 0x03 },

  /* --- AEC / AGC -------------------------------------------------------- */
  { 0x3A13, 0x43 },                       /* pre-gain = 1.047x               */
  { 0x3A18, 0x00 },                       /* gain ceiling high byte          */
  { 0x3A19, 0xF8 },                       /* gain ceiling low byte           */
  { 0x3A02, 0x03 },                       /* 60 Hz max exposure high         */
  { 0x3A03, 0xD8 },                       /* 60 Hz max exposure low          */
  { 0x3A08, 0x01 },                       /* 50 Hz band step high            */
  { 0x3A09, 0x27 },                       /* 50 Hz band step low             */
  { 0x3A0A, 0x00 },                       /* 60 Hz band step high            */
  { 0x3A0B, 0xF6 },                       /* 60 Hz band step low             */
  { 0x3A0E, 0x03 },                       /* 50 Hz max bands                 */
  { 0x3A0D, 0x04 },                       /* 60 Hz max bands                 */
  { 0x3A14, 0x03 },                       /* 50 Hz max exposure high         */
  { 0x3A15, 0xD8 },                       /* 50 Hz max exposure low          */
  { 0x3A0F, 0x30 },                       /* stable range high limit         */
  { 0x3A10, 0x28 },                       /* stable range low limit          */
  { 0x3A1B, 0x30 },                       /* stable range high limit         */
  { 0x3A1E, 0x26 },                       /* stable range low limit          */
  { 0x3A11, 0x60 },                       /* fast zone high limit            */
  { 0x3A1F, 0x14 },                       /* fast zone low limit             */

  /* --- 50/60 Hz flicker detection --------------------------------------- */
  { 0x3C01, 0x34 },
  { 0x3C04, 0x28 },
  { 0x3C05, 0x98 },
  { 0x3C06, 0x00 },
  { 0x3C07, 0x08 },
  { 0x3C08, 0x00 },
  { 0x3C09, 0x1C },
  { 0x3C0A, 0x9C },
  { 0x3C0B, 0x40 },

  /* --- timing: full-array crop, 2x binning, VGA readout ----------------- */
  { 0x3820, 0x41 },                       /* sensor flip off, ISP flip on    */
  { 0x3821, 0x07 },                       /* mirror on, binning on           */
  { 0x3814, 0x31 },                       /* horizontal subsample x2         */
  { 0x3815, 0x31 },                       /* vertical subsample x2           */
  { 0x3800, 0x00 }, { 0x3801, 0x00 },     /* X start   = 0                   */
  { 0x3802, 0x00 }, { 0x3803, 0x04 },     /* Y start   = 4                   */
  { 0x3804, 0x0A }, { 0x3805, 0x3F },     /* X end     = 2623                */
  { 0x3806, 0x07 }, { 0x3807, 0x9B },     /* Y end     = 1947                */
  { 0x3808, 0x02 }, { 0x3809, 0x80 },     /* DVP width  = 640                */
  { 0x380A, 0x01 }, { 0x380B, 0xE0 },     /* DVP height = 480                */
  { 0x380C, 0x07 }, { 0x380D, 0x68 },     /* HTS = 1896                      */
  { 0x380E, 0x03 }, { 0x380F, 0xD8 },     /* VTS = 984                       */
  { 0x3810, 0x00 }, { 0x3811, 0x10 },     /* X offset = 16                   */
  { 0x3812, 0x00 }, { 0x3813, 0x06 },     /* Y offset = 6                    */

  /* --- black level calibration ------------------------------------------ */
  { 0x4001, 0x02 },                       /* BLC start line                  */
  { 0x4004, 0x02 },                       /* BLC line count                  */

  /* --- system / clock enables ------------------------------------------- */
  { 0x3000, 0x00 },                       /* release all blocks from reset   */
  { 0x3002, 0x1C },
  { 0x3004, 0xFF },                       /* enable all block clocks         */
  { 0x3006, 0xC3 },
  { 0x300E, 0x58 },                       /* MIPI powered down, DVP enabled  */
  { 0x302E, 0x00 },

  /* --- output format (overwritten by the RGB565 table below) ------------ */
  { 0x4300, 0x6F },                       /* RGB565                          */
  { 0x501F, 0x01 },                       /* ISP output = RGB                */

  /* --- DVP timing ------------------------------------------------------- */
  { 0x4713, 0x03 },                       /* JPEG mode 3 (unused in RGB)     */
  { 0x4407, 0x04 },                       /* JPEG quantisation scale         */
  { 0x440E, 0x00 },
  { 0x460B, 0x35 },
  { 0x460C, 0x22 },                       /* PCLK divider under manual ctrl  */
  { 0x3824, 0x02 },                       /* DVP PCLK divider = /2           */
  { 0x4837, 0x22 },                       /* DVP output timing               */

  /* --- ISP master enables ----------------------------------------------- */
  { 0x5000, 0xA7 },                       /* LENC, BPC, WPC, CIP on          */
  { 0x5001, 0xA3 },                       /* SDE, scaling, UV avg, CMX on    */

  /* --- AWB -------------------------------------------------------------- */
  { 0x5180, 0xFF }, { 0x5181, 0xF2 }, { 0x5182, 0x00 }, { 0x5183, 0x14 },
  { 0x5184, 0x25 }, { 0x5185, 0x24 }, { 0x5186, 0x09 }, { 0x5187, 0x09 },
  { 0x5188, 0x09 }, { 0x5189, 0x88 }, { 0x518A, 0x54 }, { 0x518B, 0xEE },
  { 0x518C, 0xB2 }, { 0x518D, 0x50 }, { 0x518E, 0x34 }, { 0x518F, 0x6B },
  { 0x5190, 0x46 }, { 0x5191, 0xF8 }, { 0x5192, 0x04 }, { 0x5193, 0x70 },
  { 0x5194, 0xF0 }, { 0x5195, 0xF0 }, { 0x5196, 0x03 }, { 0x5197, 0x01 },
  { 0x5198, 0x04 }, { 0x5199, 0x6C }, { 0x519A, 0x04 }, { 0x519B, 0x00 },
  { 0x519C, 0x09 }, { 0x519D, 0x2B }, { 0x519E, 0x38 },

  /* --- colour matrix ---------------------------------------------------- */
  { 0x5381, 0x1E }, { 0x5382, 0x5B }, { 0x5383, 0x08 }, { 0x5384, 0x0A },
  { 0x5385, 0x7E }, { 0x5386, 0x88 }, { 0x5387, 0x7C }, { 0x5388, 0x6C },
  { 0x5389, 0x10 }, { 0x538A, 0x01 }, { 0x538B, 0x98 },

  /* --- sharpness / denoise (CIP) ---------------------------------------- */
  { 0x5300, 0x08 }, { 0x5301, 0x30 }, { 0x5302, 0x10 }, { 0x5303, 0x00 },
  { 0x5304, 0x08 }, { 0x5305, 0x30 }, { 0x5306, 0x08 }, { 0x5307, 0x16 },
  { 0x5309, 0x08 }, { 0x530A, 0x30 }, { 0x530B, 0x04 }, { 0x530C, 0x06 },

  /* --- gamma ------------------------------------------------------------ */
  { 0x5480, 0x01 }, { 0x5481, 0x08 }, { 0x5482, 0x14 }, { 0x5483, 0x28 },
  { 0x5484, 0x51 }, { 0x5485, 0x65 }, { 0x5486, 0x71 }, { 0x5487, 0x7D },
  { 0x5488, 0x87 }, { 0x5489, 0x91 }, { 0x548A, 0x9A }, { 0x548B, 0xAA },
  { 0x548C, 0xB8 }, { 0x548D, 0xCD }, { 0x548E, 0xDD }, { 0x548F, 0xEA },
  { 0x5490, 0x1D },

  /* --- special digital effects ------------------------------------------ */
  { 0x5580, 0x02 },                       /* saturation enable               */
  { 0x5583, 0x40 },
  { 0x5584, 0x10 },
  { 0x5589, 0x10 },
  { 0x558A, 0x00 },
  { 0x558B, 0xF8 },

  /* --- lens shading correction ------------------------------------------ */
  { 0x5800, 0x23 }, { 0x5801, 0x14 }, { 0x5802, 0x0F }, { 0x5803, 0x0F },
  { 0x5804, 0x12 }, { 0x5805, 0x26 }, { 0x5806, 0x0C }, { 0x5807, 0x08 },
  { 0x5808, 0x05 }, { 0x5809, 0x05 }, { 0x580A, 0x08 }, { 0x580B, 0x0D },
  { 0x580C, 0x08 }, { 0x580D, 0x03 }, { 0x580E, 0x00 }, { 0x580F, 0x00 },
  { 0x5810, 0x03 }, { 0x5811, 0x09 }, { 0x5812, 0x07 }, { 0x5813, 0x03 },
  { 0x5814, 0x00 }, { 0x5815, 0x01 }, { 0x5816, 0x03 }, { 0x5817, 0x08 },
  { 0x5818, 0x0D }, { 0x5819, 0x08 }, { 0x581A, 0x05 }, { 0x581B, 0x06 },
  { 0x581C, 0x08 }, { 0x581D, 0x0E }, { 0x581E, 0x29 }, { 0x581F, 0x17 },
  { 0x5820, 0x11 }, { 0x5821, 0x11 }, { 0x5822, 0x15 }, { 0x5823, 0x28 },
  { 0x5824, 0x46 }, { 0x5825, 0x26 }, { 0x5826, 0x08 }, { 0x5827, 0x26 },
  { 0x5828, 0x64 }, { 0x5829, 0x26 }, { 0x582A, 0x24 }, { 0x582B, 0x22 },
  { 0x582C, 0x24 }, { 0x582D, 0x24 }, { 0x582E, 0x06 }, { 0x582F, 0x22 },
  { 0x5830, 0x40 }, { 0x5831, 0x42 }, { 0x5832, 0x24 }, { 0x5833, 0x26 },
  { 0x5834, 0x24 }, { 0x5835, 0x22 }, { 0x5836, 0x22 }, { 0x5837, 0x26 },
  { 0x5838, 0x44 }, { 0x5839, 0x24 }, { 0x583A, 0x26 }, { 0x583B, 0x28 },
  { 0x583C, 0x42 }, { 0x583D, 0xCE },

  { 0x5025, 0x00 },
};

/* -------------------------------------------------------------------------- */
/* 320x240 RGB565 over DVP                                                    */
/* -------------------------------------------------------------------------- */

/*
 * The crop window and line timing from OV5640_Common are kept (2x-binned VGA
 * readout); only the ISP scaler output size and the pixel format change.
 * 0x5001[5] (scaling enable) is already set by OV5640_Common.
 */
static const OV5640_RegVal_t OV5640_QVGA_RGB565[] =
{
  { 0x3808, 0x01 }, { 0x3809, 0x40 },     /* DVP output width  = 320         */
  { 0x380A, 0x00 }, { 0x380B, 0xF0 },     /* DVP output height = 240         */

  { 0x4300, OV5640_REG_4300_RGB565 },     /* RGB565, byte order from conf.h  */
  { 0x501F, 0x01 },                       /* ISP output = RGB                */
  { 0x3821, 0x07 },                       /* keep mirror + binning enabled   */
};

#ifdef __cplusplus
}
#endif

#endif /* __OV5640_REGS_H */
