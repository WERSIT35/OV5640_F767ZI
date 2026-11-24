/**
  ******************************************************************************
  * @file    ov5640.c
  * @brief   OV5640 sensor driver (SCCB register access + configuration).
  ******************************************************************************
  */

#include "ov5640.h"
#include "ov5640_regs.h"

/* MX_I2C2_Init() owns this handle; it lives in main.c. */
extern I2C_HandleTypeDef OV5640_I2C_HANDLE;

/* -------------------------------------------------------------------------- */
/* Low-level SCCB access                                                      */
/* -------------------------------------------------------------------------- */

/*
 * SCCB on the OV5640 is plain I2C with a 16-bit register address and 8-bit
 * data, so HAL_I2C_Mem_* maps onto it directly.
 */

HAL_StatusTypeDef OV5640_WriteReg(uint16_t reg, uint8_t val)
{
  return HAL_I2C_Mem_Write(&OV5640_I2C_HANDLE, OV5640_I2C_ADDRESS,
                           reg, I2C_MEMADD_SIZE_16BIT,
                           &val, 1U, OV5640_I2C_TIMEOUT_MS);
}

HAL_StatusTypeDef OV5640_ReadReg(uint16_t reg, uint8_t *val)
{
  return HAL_I2C_Mem_Read(&OV5640_I2C_HANDLE, OV5640_I2C_ADDRESS,
                          reg, I2C_MEMADD_SIZE_16BIT,
                          val, 1U, OV5640_I2C_TIMEOUT_MS);
}

HAL_StatusTypeDef OV5640_ModifyReg(uint16_t reg, uint8_t mask, uint8_t val)
{
  uint8_t tmp;
  HAL_StatusTypeDef status = OV5640_ReadReg(reg, &tmp);

  if (status != HAL_OK)
  {
    return status;
  }

  tmp = (uint8_t)((tmp & (uint8_t)~mask) | (val & mask));

  return OV5640_WriteReg(reg, tmp);
}

/**
  * @brief  Play back a register table, honouring OV5640_REG_DELAY markers.
  */
static HAL_StatusTypeDef OV5640_WriteTable(const OV5640_RegVal_t *table,
                                           uint32_t count)
{
  for (uint32_t i = 0U; i < count; i++)
  {
    if (table[i].reg == OV5640_REG_DELAY)
    {
      HAL_Delay(table[i].val);
      continue;
    }

    if (OV5640_WriteReg(table[i].reg, table[i].val) != HAL_OK)
    {
      return HAL_ERROR;
    }
  }

  return HAL_OK;
}

/* -------------------------------------------------------------------------- */
/* Power and identification                                                   */
/* -------------------------------------------------------------------------- */

void OV5640_PowerUp(void)
{
  /* Hold both asserted, then release PWDN before RESET as the datasheet
   * power-up timing diagram requires. */
  HAL_GPIO_WritePin(OV5640_PWDN_PORT,  OV5640_PWDN_PIN,  GPIO_PIN_SET);
  HAL_GPIO_WritePin(OV5640_RESET_PORT, OV5640_RESET_PIN, GPIO_PIN_RESET);
  HAL_Delay(OV5640_T_PWDN_MS);

  HAL_GPIO_WritePin(OV5640_PWDN_PORT, OV5640_PWDN_PIN, GPIO_PIN_RESET);
  HAL_Delay(OV5640_T_PWDN_MS);

  HAL_GPIO_WritePin(OV5640_RESET_PORT, OV5640_RESET_PIN, GPIO_PIN_SET);
  HAL_Delay(OV5640_T_RESET_MS);
}

void OV5640_PowerDown(void)
{
  HAL_GPIO_WritePin(OV5640_RESET_PORT, OV5640_RESET_PIN, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(OV5640_PWDN_PORT,  OV5640_PWDN_PIN,  GPIO_PIN_SET);
}

HAL_StatusTypeDef OV5640_ReadChipId(uint16_t *id)
{
  uint8_t hi = 0U;
  uint8_t lo = 0U;

  if (OV5640_ReadReg(OV5640_REG_CHIP_ID_HIGH, &hi) != HAL_OK)
  {
    return HAL_ERROR;
  }

  if (OV5640_ReadReg(OV5640_REG_CHIP_ID_LOW, &lo) != HAL_OK)
  {
    return HAL_ERROR;
  }

  *id = (uint16_t)(((uint16_t)hi << 8) | lo);

  return HAL_OK;
}

/* -------------------------------------------------------------------------- */
/* Configuration                                                              */
/* -------------------------------------------------------------------------- */

static uint16_t ov5640_last_chip_id;

uint16_t OV5640_GetLastChipId(void)
{
  return ov5640_last_chip_id;
}

OV5640_Status_t OV5640_Init(void)
{
  uint16_t id = 0U;

  ov5640_last_chip_id = 0U;

  OV5640_PowerUp();

  /* Probe before doing anything else: a bad chip ID almost always means the
   * SCCB wiring, the 24 MHz XCLK, or the power sequence is at fault, and
   * there is no point writing 300 registers into the void.
   *
   * The two failures are reported separately because they mean different
   * things: ERR_I2C is "nothing acknowledged the address at all", whereas
   * ERR_CHIP_ID is "something answered, but it was not an OV5640". */
  if (OV5640_ReadChipId(&id) != HAL_OK)
  {
    return OV5640_ERR_I2C;
  }

  ov5640_last_chip_id = id;

  if (id != OV5640_CHIP_ID)
  {
    return OV5640_ERR_CHIP_ID;
  }

  if (OV5640_WriteTable(OV5640_Common,
                        sizeof(OV5640_Common) / sizeof(OV5640_Common[0])) != HAL_OK)
  {
    return OV5640_ERR_CONFIG;
  }

  /* Patch the PLL system divider so PCLK stays inside what the DCMI can
   * sample at the current HCLK. See OV5640_PLL_SYSDIV in ov5640_conf.h. */
  if (OV5640_WriteReg(OV5640_REG_SC_PLL_CTRL1,
                      (uint8_t)(((OV5640_PLL_SYSDIV & 0x0FU) << 4) | 0x01U)) != HAL_OK)
  {
    return OV5640_ERR_CONFIG;
  }

  if (OV5640_WriteTable(OV5640_QVGA_RGB565,
                        sizeof(OV5640_QVGA_RGB565) / sizeof(OV5640_QVGA_RGB565[0])) != HAL_OK)
  {
    return OV5640_ERR_CONFIG;
  }

  /* Gain ceiling: the stock 31x turns a dim scene into coloured noise. */
  if (OV5640_WriteReg(0x3A18U, (uint8_t)(OV5640_GAIN_CEILING >> 8)) != HAL_OK)
  {
    return OV5640_ERR_CONFIG;
  }

  if (OV5640_WriteReg(0x3A19U, (uint8_t)(OV5640_GAIN_CEILING & 0xFFU)) != HAL_OK)
  {
    return OV5640_ERR_CONFIG;
  }

#if OV5640_ENABLE_TEST_PATTERN
  if (OV5640_SetTestPattern(true) != HAL_OK)
  {
    return OV5640_ERR_CONFIG;
  }
#endif

  if (OV5640_SetStreaming(true) != HAL_OK)
  {
    return OV5640_ERR_CONFIG;
  }

  /* Let AEC/AWB converge before the first frame is handed to the caller,
   * otherwise the first few captures come out black or wildly tinted. */
  HAL_Delay(OV5640_T_STREAM_MS);

  return OV5640_OK;
}

HAL_StatusTypeDef OV5640_SetStreaming(bool enable)
{
  return OV5640_WriteReg(OV5640_REG_SYSTEM_CTRL0,
                         enable ? OV5640_SYSTEM_CTRL0_STREAM
                                : OV5640_SYSTEM_CTRL0_PWDN);
}

HAL_StatusTypeDef OV5640_SetMirrorFlip(bool mirror, bool flip)
{
  /* 0x3821[2:1] mirrors the array + ISP; 0x3820[2:1] flips it.  Bit 0 of
   * 0x3821 is the binning enable and must be preserved. */
  if (OV5640_ModifyReg(OV5640_REG_TIMING_TC_REG21, 0x06U,
                       mirror ? 0x06U : 0x00U) != HAL_OK)
  {
    return HAL_ERROR;
  }

  return OV5640_ModifyReg(OV5640_REG_TIMING_TC_REG20, 0x06U,
                          flip ? 0x06U : 0x00U);
}

HAL_StatusTypeDef OV5640_SetTestPattern(bool enable)
{
  /* 0x503D[7] enables the pattern generator, [1:0] = 00 selects colour bars. */
  return OV5640_WriteReg(OV5640_REG_TEST_PATTERN, enable ? 0x80U : 0x00U);
}

HAL_StatusTypeDef OV5640_SetBrightness(int8_t level)
{
  uint8_t magnitude;
  uint8_t sign;

  if (level < -4) { level = -4; }
  if (level >  4) { level =  4; }

  magnitude = (uint8_t)((level < 0 ? -level : level) << 4);
  sign      = (level < 0) ? 0x08U : 0x00U;   /* 0x5588[3] = subtract */

  if (OV5640_ModifyReg(0x5001U, 0x80U, 0x80U) != HAL_OK)   /* SDE enable */
  {
    return HAL_ERROR;
  }

  if (OV5640_WriteReg(0x5587U, magnitude) != HAL_OK)
  {
    return HAL_ERROR;
  }

  return OV5640_ModifyReg(0x5588U, 0x08U, sign);
}

HAL_StatusTypeDef OV5640_SetContrast(int8_t level)
{
  /* 0x5585/0x5586 are the SDE Y offset and Y gain. The step of 4 per level
   * is the value OmniVision's application note uses. */
  static const uint8_t gain[9]   = { 0x14, 0x18, 0x1C, 0x1C, 0x20,
                                     0x24, 0x28, 0x2C, 0x30 };
  static const uint8_t offset[9] = { 0x14, 0x18, 0x1C, 0x1C, 0x20,
                                     0x24, 0x28, 0x2C, 0x30 };
  uint8_t index;

  if (level < -4) { level = -4; }
  if (level >  4) { level =  4; }

  index = (uint8_t)(level + 4);

  if (OV5640_ModifyReg(0x5001U, 0x80U, 0x80U) != HAL_OK)   /* SDE enable */
  {
    return HAL_ERROR;
  }

  if (OV5640_WriteReg(0x5585U, offset[index]) != HAL_OK)
  {
    return HAL_ERROR;
  }

  if (OV5640_WriteReg(0x5586U, gain[index]) != HAL_OK)
  {
    return HAL_ERROR;
  }

  return OV5640_ModifyReg(0x5588U, 0x04U, 0x04U);          /* contrast on */
}

/* -------------------------------------------------------------------------- */
/* Picture controls                                                           */
/* -------------------------------------------------------------------------- */

HAL_StatusTypeDef OV5640_SetEffect(OV5640_Effect_t effect)
{
  /* SDE control 0x5580, U/V fixed values 0x5583/0x5584, 0x5003 = 0x08.
   * The tints and mono work by replacing U and V with constants. */
  static const uint8_t table[OV5640_EFFECT_COUNT][3] =
  {
    { 0x06, 0x40, 0x2C },   /* normal: saturation + contrast only */
    { 0x1E, 0x80, 0x80 },   /* mono                               */
    { 0x1E, 0x40, 0xA0 },   /* sepia                              */
    { 0x46, 0x40, 0x28 },   /* negative                           */
    { 0x1E, 0x80, 0xC0 },   /* red tint                           */
    { 0x1E, 0x60, 0x60 },   /* green tint                         */
    { 0x1E, 0xA0, 0x40 },   /* blue tint                          */
  };

  if (effect >= OV5640_EFFECT_COUNT)
  {
    return HAL_ERROR;
  }

  if ((OV5640_ModifyReg(0x5001U, 0x80U, 0x80U) != HAL_OK) ||   /* SDE on */
      (OV5640_WriteReg(0x5580U, table[effect][0]) != HAL_OK) ||
      (OV5640_WriteReg(0x5583U, table[effect][1]) != HAL_OK) ||
      (OV5640_WriteReg(0x5584U, table[effect][2]) != HAL_OK))
  {
    return HAL_ERROR;
  }

  return OV5640_WriteReg(0x5003U, 0x08U);
}

HAL_StatusTypeDef OV5640_SetWhiteBalance(OV5640_WhiteBalance_t wb)
{
  /* Manual R, G, B gains, 0x3400..0x3405 (12-bit, high byte first). */
  static const uint8_t gains[OV5640_WB_COUNT][6] =
  {
    { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },   /* auto: unused */
    { 0x06, 0x1C, 0x04, 0x00, 0x04, 0xF3 },   /* sunny        */
    { 0x06, 0x48, 0x04, 0x00, 0x04, 0xD3 },   /* cloudy       */
    { 0x05, 0x48, 0x04, 0x00, 0x07, 0xCF },   /* office       */
    { 0x04, 0x10, 0x04, 0x00, 0x08, 0x40 },   /* home         */
  };

  if (wb >= OV5640_WB_COUNT)
  {
    return HAL_ERROR;
  }

  if (wb == OV5640_WB_AUTO)
  {
    return OV5640_WriteReg(0x3406U, 0x00U);
  }

  if (OV5640_WriteReg(0x3406U, 0x01U) != HAL_OK)
  {
    return HAL_ERROR;
  }

  for (uint16_t i = 0U; i < 6U; i++)
  {
    if (OV5640_WriteReg((uint16_t)(0x3400U + i), gains[wb][i]) != HAL_OK)
    {
      return HAL_ERROR;
    }
  }

  return HAL_OK;
}

HAL_StatusTypeDef OV5640_SetExposureComp(int8_t step)
{
  /* AEC stable-range high/low (0x3A0F/0x3A10), fast-zone high (0x3A11),
   * and the matching 0x3A1B/0x3A1E/0x3A1F, for -1.0 .. +1.0 EV. */
  static const uint16_t regs[6] = { 0x3A0F, 0x3A10, 0x3A11,
                                    0x3A1B, 0x3A1E, 0x3A1F };
  static const uint8_t  table[7][6] =
  {
    { 0x20, 0x18, 0x41, 0x20, 0x18, 0x10 },   /* -1.0 EV */
    { 0x28, 0x20, 0x51, 0x28, 0x20, 0x10 },   /* -0.7 EV */
    { 0x30, 0x28, 0x61, 0x30, 0x28, 0x10 },   /* -0.3 EV */
    { 0x38, 0x30, 0x61, 0x38, 0x30, 0x10 },   /*  0   EV */
    { 0x40, 0x38, 0x71, 0x40, 0x38, 0x10 },   /* +0.3 EV */
    { 0x48, 0x40, 0x81, 0x48, 0x40, 0x10 },   /* +0.7 EV */
    { 0x50, 0x48, 0x90, 0x50, 0x48, 0x20 },   /* +1.0 EV */
  };
  uint32_t index;

  if (step < OV5640_EV_MIN) { step = OV5640_EV_MIN; }
  if (step > OV5640_EV_MAX) { step = OV5640_EV_MAX; }

  index = (uint32_t)(step - OV5640_EV_MIN);

  for (uint32_t i = 0U; i < 6U; i++)
  {
    if (OV5640_WriteReg(regs[i], table[index][i]) != HAL_OK)
    {
      return HAL_ERROR;
    }
  }

  return HAL_OK;
}

/** Write a list of registers, stopping at the first failure. */
static HAL_StatusTypeDef ov5640_write_list(const OV5640_RegVal_t *list,
                                           uint32_t n)
{
  for (uint32_t i = 0U; i < n; i++)
  {
    if (OV5640_WriteReg(list[i].reg, list[i].val) != HAL_OK)
    {
      return HAL_ERROR;
    }
  }

  return HAL_OK;
}

HAL_StatusTypeDef OV5640_SetNightMode(bool enable)
{
  /* Longest exposure is one frame (VTS = 984 lines) normally and three in
   * night mode, where the gain ceiling is raised as well. */
  uint16_t lines = enable ? (3U * 984U) : 984U;
  uint16_t gain  = enable ? OV5640_GAIN_CEILING_NIGHT : OV5640_GAIN_CEILING;
  const OV5640_RegVal_t list[] =
  {
    { 0x3A02U, (uint8_t)(lines >> 8) }, { 0x3A03U, (uint8_t)lines },
    { 0x3A14U, (uint8_t)(lines >> 8) }, { 0x3A15U, (uint8_t)lines },
    { 0x3A18U, (uint8_t)(gain >> 8) },  { 0x3A19U, (uint8_t)gain },
  };

  if (ov5640_write_list(list, sizeof(list) / sizeof(list[0])) != HAL_OK)
  {
    return HAL_ERROR;
  }

  /* 0x3A00[2]: night mode, AEC may insert extra frame time. */
  return OV5640_ModifyReg(0x3A00U, 0x04U, enable ? 0x04U : 0x00U);
}

HAL_StatusTypeDef OV5640_SetStyle(OV5640_Style_t style)
{
  /* Colour matrix magnitudes 0x5384..0x5389 (saturation), then the largest
   * sharpening the sensor's automatic mode may apply (0x5302). */
  static const uint8_t table[OV5640_STYLE_COUNT][7] =
  {
    { 0x0D, 0x84, 0x91, 0x8A, 0x76, 0x14, 0x14 },   /* standard */
    { 0x10, 0x9C, 0xAC, 0xA2, 0x8B, 0x17, 0x1C },   /* vivid    */
    { 0x0B, 0x6C, 0x77, 0x70, 0x60, 0x10, 0x10 },   /* natural  */
  };

  if (style >= OV5640_STYLE_COUNT)
  {
    return HAL_ERROR;
  }

  const OV5640_RegVal_t list[] =
  {
    { 0x3212U, 0x03U },                  /* group 3: apply all at once */
    { 0x5381U, 0x1DU }, { 0x5382U, 0x60U }, { 0x5383U, 0x03U },
    { 0x5384U, table[style][0] }, { 0x5385U, table[style][1] },
    { 0x5386U, table[style][2] }, { 0x5387U, table[style][3] },
    { 0x5388U, table[style][4] }, { 0x5389U, table[style][5] },
    { 0x538BU, 0x98U }, { 0x538AU, 0x01U },
    { 0x5308U, 0x25U }, { 0x5302U, table[style][6] },
    { 0x3212U, 0x13U }, { 0x3212U, 0xA3U },
  };

  return ov5640_write_list(list, sizeof(list) / sizeof(list[0]));
}

HAL_StatusTypeDef OV5640_SetZoom(uint8_t level)
{
  /* Array window { x0, y0, x1, y1 }.  Readout is 2x2 binned and loses the
   * 16 x 6 ISP offset on each side, so these feed the scaler 1280x960,
   * 640x480 and 320x240.  HTS/VTS stay put, so the frame rate does too. */
  static const uint16_t win[OV5640_ZOOM_COUNT][4] =
  {
    {   0U,   4U, 2623U, 1947U },
    { 640U, 484U, 1983U, 1467U },
    { 960U, 724U, 1663U, 1227U },
  };

  if (level >= OV5640_ZOOM_COUNT)
  {
    return HAL_ERROR;
  }

  if (OV5640_WriteReg(0x3212U, 0x03U) != HAL_OK)
  {
    return HAL_ERROR;
  }

  for (uint16_t i = 0U; i < 4U; i++)
  {
    uint16_t v = win[level][i];

    if ((OV5640_WriteReg((uint16_t)(0x3800U + (2U * i)),
                         (uint8_t)(v >> 8)) != HAL_OK) ||
        (OV5640_WriteReg((uint16_t)(0x3801U + (2U * i)),
                         (uint8_t)v) != HAL_OK))
    {
      return HAL_ERROR;
    }
  }

  if (OV5640_WriteReg(0x3212U, 0x13U) != HAL_OK)
  {
    return HAL_ERROR;
  }

  return OV5640_WriteReg(0x3212U, 0xA3U);
}
