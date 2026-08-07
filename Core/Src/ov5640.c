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
