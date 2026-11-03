/**
  ******************************************************************************
  * @file    touch.c
  * @brief   XPT2046 resistive touch controller, sharing SPI1 with the LCD.
  ******************************************************************************
  */

#include "touch.h"
#include "lcd.h"
#include <stddef.h>
#include <string.h>

/* -------------------------------------------------------------------------- */
/* XPT2046 commands: S=1, channel, 12-bit, differential, PD=00                */
/* -------------------------------------------------------------------------- */

/* PD=00 powers down between conversions and keeps PENIRQ (T_IRQ) live, so
 * the pin keeps reporting presses after every read. */
#define XPT_CMD_X      0xD0U
#define XPT_CMD_Y      0x90U
#define XPT_CMD_Z1     0xB0U

/* Below this Z1 the reading is a feather touch or the finger lifting, and
 * the X/Y values are unreliable. */
#define TOUCH_Z_MIN    80U

/* Samples per axis; the median three are averaged. */
#define TOUCH_SAMPLES  5U

/* APB2/64 = 1.69 MHz while T_CS is low (XPT2046 max 2.5 MHz). */
#define TOUCH_SPI_BR   5U

/* -------------------------------------------------------------------------- */
/* Calibration, stored in the last flash sector                               */
/* -------------------------------------------------------------------------- */

/*
 * Single-bank 2 MB flash (nDBANK = 1, checked on this board): sector 11 is
 * the last 256K, far past the ~40K of code.  Erasing it stalls instruction
 * fetch for a second or two, which only happens during calibration.
 */
#define TOUCH_CAL_SECTOR    FLASH_SECTOR_11
#define TOUCH_CAL_ADDR      0x081C0000UL
#define TOUCH_CAL_MAGIC     0x54434C31UL       /* "TCL1" */

/* screen_x = ax*rx + bx*ry + cx ; screen_y = ay*rx + by*ry + cy.  Three
 * points pin down all six, so swapped or mirrored axes need no special
 * handling. */
typedef struct
{
  uint32_t magic;
  float    ax, bx, cx;
  float    ay, by, cy;
  uint32_t check;
} Touch_Cal_t;

static Touch_Cal_t touch_cal;
static bool        touch_cal_valid;

static uint32_t Touch_CalCheck(const Touch_Cal_t *cal)
{
  const uint32_t *w = (const uint32_t *)cal;
  uint32_t        sum = 0x5A5A5A5AUL;

  for (uint32_t i = 0U; i < (offsetof(Touch_Cal_t, check) / 4U); i++)
  {
    sum = (sum << 5) ^ (sum >> 27) ^ w[i];
  }

  return sum;
}

static void Touch_CalSave(void)
{
  FLASH_EraseInitTypeDef erase = {0};
  uint32_t               error = 0U;
  const uint32_t        *w     = (const uint32_t *)&touch_cal;

  touch_cal.magic = TOUCH_CAL_MAGIC;
  touch_cal.check = Touch_CalCheck(&touch_cal);

  HAL_FLASH_Unlock();

  erase.TypeErase    = FLASH_TYPEERASE_SECTORS;
  erase.Sector       = TOUCH_CAL_SECTOR;
  erase.NbSectors    = 1U;
  erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;

  if (HAL_FLASHEx_Erase(&erase, &error) == HAL_OK)
  {
    for (uint32_t i = 0U; i < (sizeof(touch_cal) / 4U); i++)
    {
      (void)HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                              TOUCH_CAL_ADDR + (i * 4U), w[i]);
    }
  }

  HAL_FLASH_Lock();

  /* Flash is cacheable: drop any stale copy of the sector. */
  SCB_InvalidateDCache_by_Addr((uint32_t *)TOUCH_CAL_ADDR, 32);
}

/* -------------------------------------------------------------------------- */
/* SPI access                                                                 */
/* -------------------------------------------------------------------------- */

static uint32_t touch_saved_cr1;

static void Touch_SpiBegin(void)
{
  while ((SPI1->SR & SPI_SR_FTLVL) != 0U) { }
  while ((SPI1->SR & SPI_SR_BSY) != 0U) { }

  touch_saved_cr1 = SPI1->CR1;

  SPI1->CR1 &= ~SPI_CR1_SPE;
  SPI1->CR2  = (SPI1->CR2 & ~SPI_CR2_DS) | (0x7U << SPI_CR2_DS_Pos)
             | SPI_CR2_FRXTH;
  SPI1->CR1  = (touch_saved_cr1 & ~(SPI_CR1_BR | SPI_CR1_SPE))
             | (TOUCH_SPI_BR << SPI_CR1_BR_Pos);
  SPI1->CR1 |= SPI_CR1_SPE;

  /* The LCD only transmits, so the receive FIFO is full of junk and has
   * overrun.  Drain it and clear OVR (read DR, then SR). */
  while ((SPI1->SR & SPI_SR_RXNE) != 0U)
  {
    (void)*(__IO uint8_t *)&SPI1->DR;
  }
  (void)SPI1->SR;

  HAL_GPIO_WritePin(T_CS_GPIO_Port, T_CS_Pin, GPIO_PIN_RESET);
}

static void Touch_SpiEnd(void)
{
  while ((SPI1->SR & SPI_SR_FTLVL) != 0U) { }
  while ((SPI1->SR & SPI_SR_BSY) != 0U) { }

  HAL_GPIO_WritePin(T_CS_GPIO_Port, T_CS_Pin, GPIO_PIN_SET);

  SPI1->CR1 &= ~SPI_CR1_SPE;
  SPI1->CR1  = touch_saved_cr1;
}

static uint8_t Touch_Xfer(uint8_t out)
{
  while ((SPI1->SR & SPI_SR_TXE) == 0U) { }
  *(__IO uint8_t *)&SPI1->DR = out;

  while ((SPI1->SR & SPI_SR_RXNE) == 0U) { }
  return *(__IO uint8_t *)&SPI1->DR;
}

/** One 12-bit conversion. */
static uint16_t Touch_Convert(uint8_t cmd)
{
  uint16_t hi;
  uint16_t lo;

  (void)Touch_Xfer(cmd);
  hi = Touch_Xfer(0x00U);
  lo = Touch_Xfer(0x00U);

  return (uint16_t)((((hi << 8) | lo) >> 3) & 0x0FFFU);
}

/** Median-of-five, averaging the middle three. */
static uint16_t Touch_Filtered(uint8_t cmd)
{
  uint16_t s[TOUCH_SAMPLES];

  for (uint32_t i = 0U; i < TOUCH_SAMPLES; i++)
  {
    s[i] = Touch_Convert(cmd);
  }

  for (uint32_t i = 1U; i < TOUCH_SAMPLES; i++)
  {
    uint16_t v = s[i];
    uint32_t j = i;

    while ((j > 0U) && (s[j - 1U] > v))
    {
      s[j] = s[j - 1U];
      j--;
    }
    s[j] = v;
  }

  return (uint16_t)((s[1] + s[2] + s[3]) / 3U);
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

bool Touch_Init(void)
{
  memcpy(&touch_cal, (const void *)TOUCH_CAL_ADDR, sizeof(touch_cal));

  touch_cal_valid = (touch_cal.magic == TOUCH_CAL_MAGIC) &&
                    (touch_cal.check == Touch_CalCheck(&touch_cal));

  return touch_cal_valid;
}

bool Touch_IsPressed(void)
{
  return HAL_GPIO_ReadPin(T_IRQ_GPIO_Port, T_IRQ_Pin) == GPIO_PIN_RESET;
}

void Touch_Read(Touch_Point_t *pt)
{
  float fx;
  float fy;

  pt->pressed = false;

  if (!Touch_IsPressed())
  {
    return;
  }

  Touch_SpiBegin();
  pt->raw_z = Touch_Convert(XPT_CMD_Z1);
  pt->raw_x = Touch_Filtered(XPT_CMD_X);
  pt->raw_y = Touch_Filtered(XPT_CMD_Y);
  Touch_SpiEnd();

  /* Reject light contact, and a finger that lifted mid-read. */
  if ((pt->raw_z < TOUCH_Z_MIN) || !Touch_IsPressed())
  {
    return;
  }

  pt->pressed = true;

  if (!touch_cal_valid)
  {
    pt->x = 0;
    pt->y = 0;
    return;
  }

  fx = (touch_cal.ax * pt->raw_x) + (touch_cal.bx * pt->raw_y) + touch_cal.cx;
  fy = (touch_cal.ay * pt->raw_x) + (touch_cal.by * pt->raw_y) + touch_cal.cy;

  if (fx < 0.0f) { fx = 0.0f; }
  if (fy < 0.0f) { fy = 0.0f; }
  if (fx > (float)(LCD_WIDTH  - 1U)) { fx = (float)(LCD_WIDTH  - 1U); }
  if (fy > (float)(LCD_HEIGHT - 1U)) { fy = (float)(LCD_HEIGHT - 1U); }

  pt->x = (int16_t)fx;
  pt->y = (int16_t)fy;
}

/** Draw or erase a crosshair centred on (x, y). */
static void Touch_Crosshair(int16_t x, int16_t y, uint16_t colour)
{
  Lcd_FillRect((uint16_t)(x - 10), (uint16_t)(y - 1), 21U, 3U, colour);
  Lcd_FillRect((uint16_t)(x - 1), (uint16_t)(y - 10), 3U, 21U, colour);
}

/** Wait for a firm press, average it, then wait for release. */
static void Touch_SamplePoint(float *rx, float *ry)
{
  Touch_Point_t pt;
  uint32_t      sum_x = 0U;
  uint32_t      sum_y = 0U;
  uint32_t      n     = 0U;

  /* Ignore a finger still down from the previous point. */
  while (Touch_IsPressed()) { HAL_Delay(10U); }
  HAL_Delay(150U);

  while (n < 16U)
  {
    Touch_Read(&pt);

    if (pt.pressed)
    {
      sum_x += pt.raw_x;
      sum_y += pt.raw_y;
      n++;
    }

    HAL_Delay(10U);
  }

  while (Touch_IsPressed()) { HAL_Delay(10U); }
  HAL_Delay(150U);

  *rx = (float)sum_x / (float)n;
  *ry = (float)sum_y / (float)n;
}

void Touch_Calibrate(void)
{
  /* Spread over the screen but clear of the edges, where resistive panels
   * are least linear. */
  static const int16_t sx[3] = { 32, 288, 160 };
  static const int16_t sy[3] = { 32, 120, 208 };
  float rx[3];
  float ry[3];
  float det;

  Lcd_FillAsync(LCD_BLACK);
  Lcd_WaitIdle();

  for (uint32_t i = 0U; i < 3U; i++)
  {
    Touch_Crosshair(sx[i], sy[i], LCD_WHITE);
    Touch_SamplePoint(&rx[i], &ry[i]);
    Touch_Crosshair(sx[i], sy[i], LCD_GREEN);
    HAL_Delay(200U);
  }

  det = ((rx[0] - rx[2]) * (ry[1] - ry[2])) -
        ((rx[1] - rx[2]) * (ry[0] - ry[2]));

  /* Three taps on a line (or the same spot) cannot be solved. Keep any
   * older calibration rather than save garbage. */
  if ((det > -1.0f) && (det < 1.0f))
  {
    Lcd_FillAsync(LCD_RED);
    Lcd_WaitIdle();
    HAL_Delay(800U);
    return;
  }

  touch_cal.ax = (((sx[0] - sx[2]) * (ry[1] - ry[2])) -
                  ((sx[1] - sx[2]) * (ry[0] - ry[2]))) / det;
  touch_cal.bx = (((rx[0] - rx[2]) * (sx[1] - sx[2])) -
                  ((rx[1] - rx[2]) * (sx[0] - sx[2]))) / det;
  touch_cal.cx = sx[2] - (touch_cal.ax * rx[2]) - (touch_cal.bx * ry[2]);

  touch_cal.ay = (((sy[0] - sy[2]) * (ry[1] - ry[2])) -
                  ((sy[1] - sy[2]) * (ry[0] - ry[2]))) / det;
  touch_cal.by = (((rx[0] - rx[2]) * (sy[1] - sy[2])) -
                  ((rx[1] - rx[2]) * (sy[0] - sy[2]))) / det;
  touch_cal.cy = sy[2] - (touch_cal.ay * rx[2]) - (touch_cal.by * ry[2]);

  touch_cal_valid = true;

  Touch_CalSave();

  Lcd_FillAsync(LCD_GREEN);
  Lcd_WaitIdle();
  HAL_Delay(400U);
}
