/**
  ******************************************************************************
  * @file    power.c
  * @brief   Battery gauge and die temperature on ADC1 (register level).
  ******************************************************************************
  */

#include "power.h"
#include "main.h"

#define POWER_CH_BATTERY     3U    /* PA3, ADC1_IN3                         */
#define POWER_CH_VREFINT     17U
#define POWER_CH_TEMP        18U   /* F76x: temperature sensor on ADC1_IN18 */

#define POWER_POLL_MS        500U
#define POWER_DIVIDER        2U    /* 100k / 100k                           */
#define POWER_NO_BATTERY_MV  2500U /* below this, nothing is connected      */

static uint32_t power_batt_mv;
static int      power_temp_c;
static bool     power_has_battery;
static bool     power_primed;

/* Raw ADC codes from the last poll (VREFINT, battery, temperature), for
 * checking the gauge over SWD. */
volatile uint32_t power_raw[3];

/** One conversion of `channel`, 480-cycle sample time (~18 us at 27 MHz). */
static uint32_t power_convert(uint32_t channel)
{
  ADC1->SQR3 = channel;
  ADC1->SR   = 0U;
  ADC1->CR2 |= ADC_CR2_SWSTART;

  while ((ADC1->SR & ADC_SR_EOC) == 0U) { }

  return ADC1->DR & 0x0FFFU;
}

static uint32_t power_average(uint32_t channel, uint32_t n)
{
  uint32_t sum = 0U;

  for (uint32_t i = 0U; i < n; i++)
  {
    sum += power_convert(channel);
  }

  return sum / n;
}

/** Li-ion open-circuit voltage to state of charge, linear in between. */
static int power_mv_to_percent(uint32_t mv)
{
  static const uint16_t table[][2] =
  {
    { 4200, 100 }, { 4100, 90 }, { 4000, 79 }, { 3900, 66 },
    { 3800,  52 }, { 3750, 42 }, { 3700, 30 }, { 3650, 20 },
    { 3600,  12 }, { 3500,  5 }, { 3300,  0 },
  };
  const uint32_t n = sizeof(table) / sizeof(table[0]);

  if (mv >= table[0][0])     { return 100; }
  if (mv <= table[n - 1][0]) { return 0;   }

  for (uint32_t i = 1U; i < n; i++)
  {
    if (mv >= table[i][0])
    {
      uint32_t span_mv = table[i - 1][0] - table[i][0];
      uint32_t span_pc = table[i - 1][1] - table[i][1];

      return (int)(table[i][1] + (((mv - table[i][0]) * span_pc) / span_mv));
    }
  }

  return 0;
}

void Power_Init(void)
{
  __HAL_RCC_ADC1_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /* ADCCLK = PCLK2 / 4 = 27 MHz (max 36).  Temperature sensor and VREFINT
   * on; VBATE off, since VBAT shares channel 18 with the sensor. */
  ADC123_COMMON->CCR = (ADC123_COMMON->CCR &
                        ~(ADC_CCR_ADCPRE | ADC_CCR_VBATE))
                     | ADC_CCR_ADCPRE_0 | ADC_CCR_TSVREFE;

  ADC1->CR1  = 0U;                       /* 12-bit, no scan              */
  ADC1->CR2  = ADC_CR2_ADON;             /* single conversion, SW start  */
  ADC1->SQR1 = 0U;                       /* one conversion per start     */

  /* 480 cycles everywhere: the divider is a 50k source, and the sensor
   * needs >= 10 us. */
  ADC1->SMPR1 = (7U << ADC_SMPR1_SMP17_Pos) | (7U << ADC_SMPR1_SMP18_Pos);
  ADC1->SMPR2 = (7U << ADC_SMPR2_SMP3_Pos);

  HAL_Delay(1U);                         /* ADC and sensor start-up      */

  power_primed = false;
  Power_Poll();
}

void Power_Poll(void)
{
  static uint32_t last;
  uint32_t        now = HAL_GetTick();
  uint32_t        vref_raw;
  uint32_t        vdda_mv;
  uint32_t        batt_raw;
  uint32_t        temp_raw;
  uint32_t        mv;
  int32_t         cal1;
  int32_t         cal2;

  if (power_primed && ((now - last) < POWER_POLL_MS))
  {
    return;
  }

  last = now;

  /* Actual VDDA from the factory-calibrated internal reference. */
  vref_raw = power_average(POWER_CH_VREFINT, 4U);
  power_raw[0] = vref_raw;
  vdda_mv  = (vref_raw > 0U)
           ? ((3300U * (uint32_t)*VREFINT_CAL_ADDR_CMSIS) / vref_raw)
           : 3300U;

  /* Discharge PA3 for a moment so an unconnected pin reads ~0 instead of
   * whatever charge it last held. */
  GPIOA->MODER = (GPIOA->MODER & ~(3U << (POWER_CH_BATTERY * 2U)))
               | (1U << (POWER_CH_BATTERY * 2U));
  GPIOA->BSRR  = (1U << (POWER_CH_BATTERY + 16U));
  for (volatile uint32_t i = 0U; i < 50U; i++) { }
  GPIOA->MODER |= (3U << (POWER_CH_BATTERY * 2U));

  batt_raw = power_average(POWER_CH_BATTERY, 8U);
  power_raw[1] = batt_raw;
  mv       = ((batt_raw * vdda_mv) / 4095U) * POWER_DIVIDER;

  power_has_battery = (mv >= POWER_NO_BATTERY_MV);

  /* Smooth out load ripple; snap on first reading or when a battery is
   * first connected. */
  if (!power_primed || (power_batt_mv < POWER_NO_BATTERY_MV))
  {
    power_batt_mv = mv;
  }
  else
  {
    power_batt_mv = ((power_batt_mv * 7U) + mv) / 8U;
  }

  /* Die temperature, using the two factory calibration points. */
  power_raw[2] = power_average(POWER_CH_TEMP, 4U);
  temp_raw = (power_raw[2] * vdda_mv) / 3300U;
  cal1     = (int32_t)*TEMPSENSOR_CAL1_ADDR_CMSIS;
  cal2     = (int32_t)*TEMPSENSOR_CAL2_ADDR_CMSIS;

  if (cal2 != cal1)
  {
    power_temp_c = 30 + (int)((((int32_t)temp_raw - cal1) * (110 - 30))
                              / (cal2 - cal1));
  }

  power_primed = true;
}

bool Power_HasBattery(void)
{
  return power_has_battery;
}

int Power_GetPercent(void)
{
  return power_mv_to_percent(power_batt_mv);
}

uint32_t Power_GetBatteryMv(void)
{
  return power_batt_mv;
}

int Power_GetTempC(void)
{
  return power_temp_c;
}
