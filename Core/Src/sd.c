/**
  ******************************************************************************
  * @file    sd.c
  * @brief   SD card driver (SPI mode, CRC on) and the FatFs disk interface.
  ******************************************************************************
  */

#pragma GCC optimize ("O2")

#include "sd.h"
#include "lcd.h"
#include "main.h"
#include "diskio.h"

#define SD_POLL_MS     2000U
#define SD_TRIES       4U

/* APB2 (108 MHz) dividers: /256 = 422 kHz to initialise, then /8 = 13.5 MHz,
 * halved each time a transfer fails its CRC. */
#define SD_BR_INIT     7U
#define SD_BR_FAST     2U
#define SD_BR_SLOWEST  5U

#define TOKEN_SINGLE   0xFEU
#define TOKEN_MULTI    0xFCU
#define TOKEN_STOP     0xFDU

/* Counters for checking the wiring over SWD. */
typedef struct
{
  uint32_t reads;          /* blocks                                         */
  uint32_t writes;
  uint32_t crc_errors;     /* blocks that arrived damaged                    */
  uint32_t retries;
  uint32_t failures;       /* transfers given up on                          */
  uint32_t sectors;        /* card size in 512-byte sectors                  */
  uint32_t br;             /* SPI divider in use                             */
  uint32_t refused;        /* writes blocked by the gate                     */
} Sd_Stats_t;

volatile Sd_Stats_t sd_stats;

static bool     sd_present;
static bool     sd_high_capacity;
static bool     sd_write_ok;
static uint32_t sd_protected;
static uint32_t sd_br = SD_BR_INIT;
static uint32_t sd_cr1;

/* -------------------------------------------------------------------------- */
/* CRC                                                                        */
/* -------------------------------------------------------------------------- */

static uint16_t crc16_table[256];

static void crc16_init(void)
{
  for (uint32_t i = 0U; i < 256U; i++)
  {
    uint16_t c = (uint16_t)(i << 8);

    for (int b = 0; b < 8; b++)
    {
      c = (uint16_t)((c & 0x8000U) ? ((c << 1) ^ 0x1021U) : (c << 1));
    }

    crc16_table[i] = c;
  }
}

static uint16_t crc16(const uint8_t *p, uint32_t n)
{
  uint16_t c = 0U;

  while (n-- > 0U)
  {
    c = (uint16_t)((c << 8) ^ crc16_table[(c >> 8) ^ *p++]);
  }

  return c;
}

static uint8_t crc7(const uint8_t *p, uint32_t n)
{
  uint8_t c = 0U;

  while (n-- > 0U)
  {
    uint8_t d = *p++;

    for (int b = 0; b < 8; b++)
    {
      c = (uint8_t)(c << 1);
      if (((d ^ c) & 0x80U) != 0U) { c ^= 0x09U; }
      d = (uint8_t)(d << 1);
    }
  }

  return (uint8_t)(c & 0x7FU);
}

/* -------------------------------------------------------------------------- */
/* Bus                                                                        */
/* -------------------------------------------------------------------------- */

static inline uint8_t xfer(uint8_t out)
{
  while ((SPI1->SR & SPI_SR_TXE) == 0U) { }
  *(__IO uint8_t *)&SPI1->DR = out;

  while ((SPI1->SR & SPI_SR_RXNE) == 0U) { }
  return *(__IO uint8_t *)&SPI1->DR;
}

static void cs(bool active)
{
  HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin,
                    active ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static void bus_begin(void)
{
  Lcd_WaitIdle();

  while ((SPI1->SR & SPI_SR_FTLVL) != 0U) { }
  while ((SPI1->SR & SPI_SR_BSY) != 0U) { }

  /* SD cards need MISO pulled up; the display module may not do it. */
  GPIOB->PUPDR = (GPIOB->PUPDR & ~GPIO_PUPDR_PUPDR4) | GPIO_PUPDR_PUPDR4_0;

  sd_cr1 = SPI1->CR1;

  SPI1->CR1 &= ~SPI_CR1_SPE;
  SPI1->CR2  = (SPI1->CR2 & ~SPI_CR2_DS) | (0x7U << SPI_CR2_DS_Pos)
             | SPI_CR2_FRXTH;
  SPI1->CR1  = (sd_cr1 & ~(SPI_CR1_BR | SPI_CR1_SPE))
             | (sd_br << SPI_CR1_BR_Pos);
  SPI1->CR1 |= SPI_CR1_SPE;

  /* Drop whatever the LCD traffic left in the receive FIFO. */
  while ((SPI1->SR & SPI_SR_RXNE) != 0U)
  {
    (void)*(__IO uint8_t *)&SPI1->DR;
  }
  (void)SPI1->SR;
}

static void bus_end(void)
{
  /* One byte with CS high so the card releases MISO for the touch chip. */
  cs(false);
  (void)xfer(0xFFU);

  while ((SPI1->SR & SPI_SR_FTLVL) != 0U) { }
  while ((SPI1->SR & SPI_SR_BSY) != 0U) { }

  SPI1->CR1 &= ~SPI_CR1_SPE;
  SPI1->CR1  = sd_cr1;
}

/** Wait for the card to stop holding MISO low. */
static bool wait_ready(uint32_t ms)
{
  uint32_t start = HAL_GetTick();

  do
  {
    if (xfer(0xFFU) == 0xFFU)
    {
      return true;
    }
  } while ((HAL_GetTick() - start) < ms);

  return false;
}

/** Send a command (CS already low) and return R1, or 0xFF for no answer. */
static uint8_t command(uint8_t cmd, uint32_t arg)
{
  uint8_t f[5];
  uint8_t r = 0xFFU;

  if ((cmd != 0U) && (cmd != 12U) && !wait_ready(500U))
  {
    return 0xFFU;
  }

  f[0] = (uint8_t)(0x40U | cmd);
  f[1] = (uint8_t)(arg >> 24);
  f[2] = (uint8_t)(arg >> 16);
  f[3] = (uint8_t)(arg >> 8);
  f[4] = (uint8_t)arg;

  for (uint32_t i = 0U; i < 5U; i++) { (void)xfer(f[i]); }
  (void)xfer((uint8_t)((crc7(f, 5U) << 1) | 1U));

  if (cmd == 12U)
  {
    (void)xfer(0xFFU);                /* stuff byte */
  }

  for (uint32_t i = 0U; i < 10U; i++)
  {
    r = xfer(0xFFU);
    if ((r & 0x80U) == 0U) { break; }
  }

  return r;
}

/** Start a fresh command: reselect the card, then send. */
static uint8_t command_new(uint8_t cmd, uint32_t arg)
{
  cs(false);
  (void)xfer(0xFFU);
  cs(true);
  (void)xfer(0xFFU);
  return command(cmd, arg);
}

static uint8_t acommand(uint8_t cmd, uint32_t arg)
{
  (void)command_new(55U, 0U);
  return command_new(cmd, arg);
}

/* -------------------------------------------------------------------------- */
/* Blocks                                                                     */
/* -------------------------------------------------------------------------- */

static bool get_block(uint8_t *buf, uint32_t n)
{
  uint32_t start = HAL_GetTick();
  uint8_t  t;
  uint16_t crc;

  do
  {
    t = xfer(0xFFU);
  } while ((t == 0xFFU) && ((HAL_GetTick() - start) < 300U));

  if (t != TOKEN_SINGLE)
  {
    return false;
  }

  for (uint32_t i = 0U; i < n; i++) { buf[i] = xfer(0xFFU); }

  crc  = (uint16_t)(xfer(0xFFU) << 8);
  crc |= xfer(0xFFU);

  if (crc != crc16(buf, n))
  {
    sd_stats.crc_errors++;
    return false;
  }

  return true;
}

static bool put_block(const uint8_t *buf, uint8_t token)
{
  uint16_t crc = crc16(buf, 512U);
  uint8_t  r;

  if (!wait_ready(1000U))
  {
    return false;
  }

  (void)xfer(token);
  for (uint32_t i = 0U; i < 512U; i++) { (void)xfer(buf[i]); }
  (void)xfer((uint8_t)(crc >> 8));
  (void)xfer((uint8_t)crc);

  r = (uint8_t)(xfer(0xFFU) & 0x1FU);

  if (r == 0x0BU)
  {
    sd_stats.crc_errors++;
  }

  return r == 0x05U;
}

static uint32_t address(uint32_t sector)
{
  return sd_high_capacity ? sector : (sector * 512U);
}

static bool read_once(uint8_t *buf, uint32_t sector, uint32_t count)
{
  bool ok = true;

  if (count == 1U)
  {
    return (command_new(17U, address(sector)) == 0U) && get_block(buf, 512U);
  }

  if (command_new(18U, address(sector)) != 0U)
  {
    return false;
  }

  while (ok && (count-- > 0U))
  {
    ok   = get_block(buf, 512U);
    buf += 512;
  }

  (void)command(12U, 0U);
  return wait_ready(500U) && ok;
}

static bool write_once(const uint8_t *buf, uint32_t sector, uint32_t count)
{
  bool ok = true;

  if (count == 1U)
  {
    ok = (command_new(24U, address(sector)) == 0U) &&
         (xfer(0xFFU) == 0xFFU) && put_block(buf, TOKEN_SINGLE);
  }
  else
  {
    if (command_new(25U, address(sector)) != 0U)
    {
      return false;
    }

    (void)xfer(0xFFU);

    while (ok && (count-- > 0U))
    {
      ok   = put_block(buf, TOKEN_MULTI);
      buf += 512;
    }

    if (ok)
    {
      ok = wait_ready(1000U);
      (void)xfer(TOKEN_STOP);
      (void)xfer(0xFFU);
    }
    else
    {
      (void)command(12U, 0U);
    }
  }

  if (!wait_ready(1000U))
  {
    return false;
  }

  /* The card reports programming errors only when asked. */
  if (ok)
  {
    ok = (command_new(13U, 0U) == 0U) && (xfer(0xFFU) == 0U);
  }

  return ok;
}

static bool transfer(uint8_t *rd, const uint8_t *wr, uint32_t sector,
                     uint32_t count)
{
  for (uint32_t t = 0U; t < SD_TRIES; t++)
  {
    bool ok;

    bus_begin();
    ok = (rd != 0) ? read_once(rd, sector, count)
                   : write_once(wr, sector, count);
    bus_end();

    if (ok)
    {
      if (rd != 0) { sd_stats.reads += count; } else { sd_stats.writes += count; }
      return true;
    }

    sd_stats.retries++;

    if (sd_br < SD_BR_SLOWEST)
    {
      sd_br++;
      sd_stats.br = sd_br;
    }
  }

  sd_stats.failures++;
  return false;
}

/* -------------------------------------------------------------------------- */
/* Card                                                                       */
/* -------------------------------------------------------------------------- */

static uint32_t csd_sectors(const uint8_t *c)
{
  if ((c[0] >> 6) == 1U)
  {
    uint32_t size = ((uint32_t)(c[7] & 0x3FU) << 16) |
                    ((uint32_t)c[8] << 8) | c[9];
    return (size + 1U) * 1024U;
  }
  else
  {
    uint32_t size = ((uint32_t)(c[6] & 3U) << 10) | ((uint32_t)c[7] << 2) |
                    (c[8] >> 6);
    uint32_t mult = ((c[9] & 3U) << 1) | (c[10] >> 7);
    uint32_t len  = c[5] & 0x0FU;

    return (size + 1U) << (mult + 2U + len - 9U);
  }
}

static bool card_init(void)
{
  uint8_t  r   = 0xFFU;
  uint8_t  x[16];
  bool     v2;
  bool     ok  = false;
  uint32_t start;

  if (crc16_table[1] == 0U) { crc16_init(); }

  sd_br = SD_BR_INIT;
  bus_begin();

  for (uint32_t t = 0U; (t < 5U) && (r != 0x01U); t++)
  {
    cs(false);
    for (uint32_t i = 0U; i < 10U; i++) { (void)xfer(0xFFU); }
    cs(true);
    (void)xfer(0xFFU);
    r = command(0U, 0U);
  }

  if (r == 0x01U)
  {
    r  = command_new(8U, 0x1AAU);
    v2 = (r == 0x01U);

    if (v2)
    {
      for (uint32_t i = 0U; i < 4U; i++) { x[i] = xfer(0xFFU); }
      v2 = (x[2] == 0x01U) && (x[3] == 0xAAU);
    }

    /* From here the card checks our CRCs too. */
    (void)command_new(59U, 1U);

    start = HAL_GetTick();

    do
    {
      r = acommand(41U, v2 ? 0x40000000UL : 0U);
    } while ((r != 0U) && (r != 0xFFU) && ((HAL_GetTick() - start) < 1000U));

    if (r == 0U)
    {
      sd_high_capacity = false;

      if (v2 && (command_new(58U, 0U) == 0U))
      {
        for (uint32_t i = 0U; i < 4U; i++) { x[i] = xfer(0xFFU); }
        sd_high_capacity = (x[0] & 0x40U) != 0U;
      }

      if (!sd_high_capacity)
      {
        (void)command_new(16U, 512U);
      }

      if ((command_new(9U, 0U) == 0U) && get_block(x, 16U))
      {
        sd_stats.sectors = csd_sectors(x);
        ok = true;
      }
    }
  }

  bus_end();

  if (ok)
  {
    sd_br = SD_BR_FAST;
  }

  sd_stats.br = sd_br;
  return ok;
}

static bool card_alive(void)
{
  bool ok;

  bus_begin();
  ok = (command_new(13U, 0U) == 0U) && (xfer(0xFFU) == 0U);
  bus_end();

  return ok;
}

void Sd_Poll(void)
{
  static uint32_t last;
  static bool     first = true;
  uint32_t        now   = HAL_GetTick();

  if (!first && ((now - last) < SD_POLL_MS))
  {
    return;
  }

  first = false;

  if (sd_present)
  {
    sd_present = card_alive();
  }

  if (!sd_present)
  {
    sd_present = card_init();
  }

  last = HAL_GetTick();
}

bool Sd_IsPresent(void)
{
  return sd_present;
}

void Sd_AllowWrite(bool allow)
{
  sd_write_ok = allow;
}

void Sd_Protect(uint32_t sector)
{
  sd_protected = sector;
}

/* -------------------------------------------------------------------------- */
/* FatFs disk interface                                                       */
/* -------------------------------------------------------------------------- */

DSTATUS disk_status(BYTE pdrv)
{
  (void)pdrv;
  return sd_present ? 0U : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv)
{
  return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
  (void)pdrv;

  if (!sd_present)
  {
    return RES_NOTRDY;
  }

  return transfer(buff, 0, sector, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, DWORD sector, UINT count)
{
  (void)pdrv;

  if (!sd_present)
  {
    return RES_NOTRDY;
  }

  /* Gate closed, the partition table, the boot sector, or past the end. */
  if (!sd_write_ok || (sector == 0U) ||
      ((sd_protected >= sector) && (sd_protected < (sector + count))) ||
      ((sector + count) > sd_stats.sectors))
  {
    sd_stats.refused++;
    return RES_WRPRT;
  }

  return transfer(0, buff, sector, count) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
  (void)pdrv;

  switch (cmd)
  {
    case CTRL_SYNC:                   /* every write already waited */
      return RES_OK;

    case GET_SECTOR_COUNT:
      *(DWORD *)buff = sd_stats.sectors;
      return RES_OK;

    default:
      return RES_PARERR;
  }
}
