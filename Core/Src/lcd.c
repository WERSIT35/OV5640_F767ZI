/**
  ******************************************************************************
  * @file    lcd.c
  * @brief   ILI9341 320x240 TFT on SPI1, with DMA2 Stream3 full-frame blits.
  ******************************************************************************
  */

#include "lcd.h"

/* MX_SPI1_Init() owns this handle; it lives in main.c.  CubeMX sets SPI1 up
 * (pins, mode, 8-bit frames); everything after that is done on the
 * registers here, because the transfers are too short for HAL's per-call
 * overhead to be worth paying and the data size flips between 8 and 16 bit
 * on every frame. */
extern SPI_HandleTypeDef hspi1;

/* -------------------------------------------------------------------------- */
/* ILI9341 commands                                                           */
/* -------------------------------------------------------------------------- */

#define ILI_SWRESET   0x01U
#define ILI_SLPOUT    0x11U
#define ILI_GAMSET    0x26U
#define ILI_DISPON    0x29U
#define ILI_CASET     0x2AU
#define ILI_PASET     0x2BU
#define ILI_RAMWR     0x2CU
#define ILI_MADCTL    0x36U
#define ILI_PIXFMT    0x3AU
#define ILI_FRMCTR1   0xB1U
#define ILI_DFUNCTR   0xB6U
#define ILI_PWCTR1    0xC0U
#define ILI_PWCTR2    0xC1U
#define ILI_VMCTR1    0xC5U
#define ILI_VMCTR2    0xC7U
#define ILI_GMCTRP1   0xE0U
#define ILI_GMCTRN1   0xE1U

/* -------------------------------------------------------------------------- */
/* DMA: SPI1_TX is DMA2 Stream3 Channel3 (Stream1 is taken by the DCMI)       */
/* -------------------------------------------------------------------------- */

#define LCD_DMA_STREAM       DMA2_Stream3
#define LCD_DMA_IRQn         DMA2_Stream3_IRQn

/* Stream3's flags in LISR/LIFCR: FEIF3 (22), DMEIF3 (24), TEIF3 (25),
 * HTIF3 (26), TCIF3 (27). */
#define LCD_DMA_FLAGS        (DMA_LIFCR_CFEIF3 | DMA_LIFCR_CDMEIF3 | \
                              DMA_LIFCR_CTEIF3 | DMA_LIFCR_CHTIF3  | \
                              DMA_LIFCR_CTCIF3)

/* NDTR is 16 bits, a frame is 76,800 pixels: send it as two halves. */
#define LCD_FRAME_PIXELS     (LCD_WIDTH * LCD_HEIGHT)
#define LCD_DMA_CHUNK        (LCD_FRAME_PIXELS / 2U)

/* -------------------------------------------------------------------------- */
/* State                                                                      */
/* -------------------------------------------------------------------------- */

static volatile bool     lcd_busy;
static volatile uint32_t lcd_frame_count;
static volatile uint32_t lcd_dma_errors;

/* Next chunk of the transfer in flight. */
static volatile uint32_t lcd_next_addr;
static volatile uint32_t lcd_remaining;
static volatile bool     lcd_minc;

/* Source of a solid fill: one pixel, sent 76,800 times with MINC off.  It
 * sits in .bss, i.e. DTCM, which is never cached, so the DMA always sees
 * the value just written. */
static volatile uint16_t lcd_fill_colour;

/* -------------------------------------------------------------------------- */
/* Pin helpers                                                                */
/* -------------------------------------------------------------------------- */

static inline void lcd_cs(bool active)
{
  LCD_CS_GPIO_Port->BSRR = active ? ((uint32_t)LCD_CS_Pin << 16) : LCD_CS_Pin;
}

static inline void lcd_dc_data(bool data)
{
  LCD_DC_GPIO_Port->BSRR = data ? LCD_DC_Pin : ((uint32_t)LCD_DC_Pin << 16);
}

/* -------------------------------------------------------------------------- */
/* SPI register helpers                                                       */
/* -------------------------------------------------------------------------- */

/** CR1.BR encoding of LCD_SPI_DIV: divide by 2^(BR+1). */
static uint32_t lcd_spi_br_bits(void)
{
  uint32_t br = 0U;

  while (((2UL << br) < LCD_SPI_DIV) && (br < 7U))
  {
    br++;
  }

  return br;
}

/** Wait until every bit queued so far has left the shift register. */
static void spi_wait_idle(void)
{
  while ((SPI1->SR & SPI_SR_FTLVL) != 0U) { }
  while ((SPI1->SR & SPI_SR_BSY) != 0U) { }
}

/**
  * @brief  Drop whatever the full-duplex receiver collected while we were
  *         only transmitting, and clear the resulting overrun.
  * @note   Harmless for the panel, but the touch controller shares MISO and
  *         must not read stale bytes.
  */
static void spi_flush_rx(void)
{
  while ((SPI1->SR & SPI_SR_RXNE) != 0U)
  {
    (void)*(__IO uint8_t *)&SPI1->DR;
  }

  (void)SPI1->SR;
}

/** Switch between 8-bit command bytes and 16-bit pixel frames. */
static void spi_set_16bit(bool wide)
{
  spi_wait_idle();

  SPI1->CR1 &= ~SPI_CR1_SPE;

  if (wide)
  {
    SPI1->CR2 = (SPI1->CR2 & ~(SPI_CR2_DS | SPI_CR2_FRXTH))
              | (0xFU << SPI_CR2_DS_Pos);
  }
  else
  {
    SPI1->CR2 = (SPI1->CR2 & ~SPI_CR2_DS)
              | (0x7U << SPI_CR2_DS_Pos) | SPI_CR2_FRXTH;
  }

  SPI1->CR1 |= SPI_CR1_SPE;
}

static void spi_write8(uint8_t value)
{
  while ((SPI1->SR & SPI_SR_TXE) == 0U) { }

  /* Byte access: a 32-bit write to DR would pack two frames on the F7. */
  *(__IO uint8_t *)&SPI1->DR = value;
}

static void spi_write16(uint16_t value)
{
  while ((SPI1->SR & SPI_SR_TXE) == 0U) { }

  *(__IO uint16_t *)&SPI1->DR = value;
}

/* -------------------------------------------------------------------------- */
/* Command layer (blocking, 8-bit)                                            */
/* -------------------------------------------------------------------------- */

/** Send one command byte; CS must already be low. */
static void lcd_write_cmd(uint8_t cmd)
{
  spi_wait_idle();
  lcd_dc_data(false);
  spi_write8(cmd);
  spi_wait_idle();
  lcd_dc_data(true);
}

static void lcd_command(uint8_t cmd, const uint8_t *data, uint32_t len)
{
  lcd_cs(true);
  lcd_write_cmd(cmd);

  for (uint32_t i = 0U; i < len; i++)
  {
    spi_write8(data[i]);
  }

  spi_wait_idle();
  lcd_cs(false);
}

/**
  * @brief  Read `len` bytes back after a command, for wiring diagnostics.
  * @note   Needs the panel's SDO wired to PB4 - normally it is left off (see
  *         lcd.h).  Reads run at APB2/32 = 3.4 MHz: the ILI9341 read cycle
  *         is far slower than its write cycle.  The first byte returned by
  *         most read commands is a dummy.
  */
static void lcd_read(uint8_t cmd, uint8_t *out, uint32_t len)
{
  uint32_t cr1 = SPI1->CR1;

  spi_set_16bit(false);
  spi_wait_idle();
  SPI1->CR1 &= ~SPI_CR1_SPE;
  SPI1->CR1  = (cr1 & ~(SPI_CR1_BR | SPI_CR1_SPE)) | (4U << SPI_CR1_BR_Pos);
  SPI1->CR1 |= SPI_CR1_SPE;

  lcd_cs(true);
  lcd_write_cmd(cmd);
  spi_flush_rx();

  for (uint32_t i = 0U; i < len; i++)
  {
    spi_write8(0x00U);
    while ((SPI1->SR & SPI_SR_RXNE) == 0U) { }
    out[i] = *(__IO uint8_t *)&SPI1->DR;
  }

  spi_wait_idle();
  lcd_cs(false);

  SPI1->CR1 &= ~SPI_CR1_SPE;
  SPI1->CR1  = cr1;
}

/*
 * Read-back of the panel's own state after Lcd_Init, for SWD inspection:
 *   [0..1] RDDPM  0x0A: dummy, power mode   - expect 0x9C (booster on,
 *                       sleep out, normal mode, display on)
 *   [2..3] RDDCOLMOD 0x0C: dummy, pixel fmt - expect 0x05/0x55 (16 bpp)
 *   [4..7] RDID4  0xD3: dummy, 0x00, 0x93, 0x41
 * All 0x00 or all 0xFF means nothing answered: SDO not wired to PB4, or
 * the panel is not receiving commands at all.
 */
volatile uint8_t lcd_diag[8];

static void lcd_read_diag(void)
{
  uint8_t buf[4];

  lcd_read(0x0AU, buf, 2U);
  lcd_diag[0] = buf[0];
  lcd_diag[1] = buf[1];

  lcd_read(0x0CU, buf, 2U);
  lcd_diag[2] = buf[0];
  lcd_diag[3] = buf[1];

  lcd_read(0xD3U, buf, 4U);
  lcd_diag[4] = buf[0];
  lcd_diag[5] = buf[1];
  lcd_diag[6] = buf[2];
  lcd_diag[7] = buf[3];
}

/**
  * @brief  Select a drawing window and issue RAMWR.
  * @note   Leaves CS asserted and DC high: the caller streams pixels next.
  */
static void lcd_begin_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
  uint16_t x1 = (uint16_t)(x + w - 1U);
  uint16_t y1 = (uint16_t)(y + h - 1U);

  spi_set_16bit(false);
  spi_flush_rx();

  lcd_cs(true);

  lcd_write_cmd(ILI_CASET);
  spi_write8((uint8_t)(x >> 8));  spi_write8((uint8_t)x);
  spi_write8((uint8_t)(x1 >> 8)); spi_write8((uint8_t)x1);

  lcd_write_cmd(ILI_PASET);
  spi_write8((uint8_t)(y >> 8));  spi_write8((uint8_t)y);
  spi_write8((uint8_t)(y1 >> 8)); spi_write8((uint8_t)y1);

  lcd_write_cmd(ILI_RAMWR);

  spi_set_16bit(true);
}

/* -------------------------------------------------------------------------- */
/* DMA                                                                        */
/* -------------------------------------------------------------------------- */

static void lcd_dma_start_chunk(void)
{
  uint32_t count = lcd_remaining;
  uint32_t cr;

  if (count > LCD_DMA_CHUNK)
  {
    count = LCD_DMA_CHUNK;
  }

  LCD_DMA_STREAM->CR &= ~DMA_SxCR_EN;
  while ((LCD_DMA_STREAM->CR & DMA_SxCR_EN) != 0U) { }

  DMA2->LIFCR = LCD_DMA_FLAGS;

  /* Channel 3, memory -> peripheral, 16-bit on both sides, high priority.
   * Bursts of four halfwords from memory keep the AHB matrix load down
   * while the DCMI stream is also writing into SRAM. A fill reads the same
   * halfword over and over, which cannot be burst, so it goes single. */
  cr = DMA_SxCR_CHSEL_0 | DMA_SxCR_CHSEL_1
     | DMA_SxCR_PL_1
     | DMA_SxCR_MSIZE_0 | DMA_SxCR_PSIZE_0
     | DMA_SxCR_DIR_0
     | DMA_SxCR_TCIE | DMA_SxCR_TEIE;

  if (lcd_minc)
  {
    cr |= DMA_SxCR_MINC | DMA_SxCR_MBURST_0;
  }

  LCD_DMA_STREAM->CR   = cr;
  LCD_DMA_STREAM->FCR  = DMA_SxFCR_DMDIS | DMA_SxFCR_FTH;
  LCD_DMA_STREAM->PAR  = (uint32_t)&SPI1->DR;
  LCD_DMA_STREAM->M0AR = lcd_next_addr;
  LCD_DMA_STREAM->NDTR = count;

  lcd_remaining -= count;

  if (lcd_minc)
  {
    lcd_next_addr += count * 2U;
  }

  LCD_DMA_STREAM->CR |= DMA_SxCR_EN;
}

static void lcd_start_frame(uint32_t addr, bool minc)
{
  /* Wait out any previous transfer rather than corrupt it. */
  Lcd_WaitIdle();

  lcd_busy = true;

  lcd_begin_window(0U, 0U, LCD_WIDTH, LCD_HEIGHT);

  lcd_next_addr = addr;
  lcd_remaining = LCD_FRAME_PIXELS;
  lcd_minc      = minc;

  SPI1->CR2 |= SPI_CR2_TXDMAEN;

  lcd_dma_start_chunk();
}

static void lcd_finish_frame(void)
{
  /* The DMA is done once the last halfword is in the TX FIFO; the panel
   * only has it once the shift register has emptied.  At 54 MHz that is
   * well under a microsecond, so waiting here in the ISR is fine. */
  spi_wait_idle();

  lcd_cs(false);
  SPI1->CR2 &= ~SPI_CR2_TXDMAEN;

  lcd_frame_count++;
  lcd_busy = false;
}

void Lcd_DmaIrqHandler(void)
{
  uint32_t lisr = DMA2->LISR;

  if ((lisr & DMA_LISR_TEIF3) != 0U)
  {
    DMA2->LIFCR = LCD_DMA_FLAGS;
    lcd_dma_errors++;
    lcd_remaining = 0U;
    lcd_finish_frame();
    return;
  }

  if ((lisr & DMA_LISR_TCIF3) != 0U)
  {
    DMA2->LIFCR = LCD_DMA_FLAGS;

    if (lcd_remaining > 0U)
    {
      lcd_dma_start_chunk();
    }
    else
    {
      lcd_finish_frame();
    }
  }
}

/* -------------------------------------------------------------------------- */
/* Public API                                                                 */
/* -------------------------------------------------------------------------- */

void Lcd_Init(void)
{
  /* Init sequence from the ILI9341 application notes, as used by most
   * drivers for these 2.8" modules.  Each entry: command, length, data. */
  static const uint8_t init_seq[] =
  {
    0xEF, 3, 0x03, 0x80, 0x02,
    0xCF, 3, 0x00, 0xC1, 0x30,              /* power control B          */
    0xED, 4, 0x64, 0x03, 0x12, 0x81,        /* power-on sequence        */
    0xE8, 3, 0x85, 0x00, 0x78,              /* driver timing control A  */
    0xCB, 5, 0x39, 0x2C, 0x00, 0x34, 0x02,  /* power control A          */
    0xF7, 1, 0x20,                          /* pump ratio               */
    0xEA, 2, 0x00, 0x00,                    /* driver timing control B  */
    ILI_PWCTR1, 1, 0x23,
    ILI_PWCTR2, 1, 0x10,
    ILI_VMCTR1, 2, 0x3E, 0x28,
    ILI_VMCTR2, 1, 0x86,
    ILI_MADCTL, 1, LCD_MADCTL,
    ILI_PIXFMT, 1, 0x55,                    /* 16 bits per pixel        */
    ILI_FRMCTR1, 2, 0x00, 0x18,             /* 79 Hz refresh            */
    ILI_DFUNCTR, 3, 0x08, 0x82, 0x27,
    0xF2, 1, 0x00,                          /* 3-gamma off              */
    ILI_GAMSET, 1, 0x01,
    ILI_GMCTRP1, 15, 0x0F, 0x31, 0x2B, 0x0C, 0x0E, 0x08, 0x4E, 0xF1,
                     0x37, 0x07, 0x10, 0x03, 0x0E, 0x09, 0x00,
    ILI_GMCTRN1, 15, 0x00, 0x0E, 0x14, 0x03, 0x11, 0x07, 0x31, 0xC1,
                     0x48, 0x08, 0x0F, 0x0C, 0x31, 0x36, 0x0F,
  };

  __HAL_RCC_DMA2_CLK_ENABLE();

  /* Clock: APB2 / LCD_SPI_DIV, see lcd.h. */
  SPI1->CR1 &= ~SPI_CR1_SPE;
  SPI1->CR1 = (SPI1->CR1 & ~SPI_CR1_BR) | (lcd_spi_br_bits() << SPI_CR1_BR_Pos);
  SPI1->CR1 |= SPI_CR1_SPE;

  spi_set_16bit(false);

  lcd_cs(false);
  lcd_dc_data(true);

  /* Hardware reset: >10 us low, then 120 ms before Sleep Out is allowed. */
  HAL_GPIO_WritePin(LCD_RST_GPIO_Port, LCD_RST_Pin, GPIO_PIN_RESET);
  HAL_Delay(10U);
  HAL_GPIO_WritePin(LCD_RST_GPIO_Port, LCD_RST_Pin, GPIO_PIN_SET);
  HAL_Delay(120U);

  lcd_command(ILI_SWRESET, NULL, 0U);
  HAL_Delay(150U);

  for (uint32_t i = 0U; i < sizeof(init_seq); )
  {
    uint8_t cmd = init_seq[i];
    uint8_t len = init_seq[i + 1U];

    lcd_command(cmd, &init_seq[i + 2U], len);
    i += 2U + len;
  }

  lcd_command(ILI_SLPOUT, NULL, 0U);
  HAL_Delay(120U);

  lcd_command(ILI_DISPON, NULL, 0U);
  HAL_Delay(20U);

  lcd_read_diag();

  HAL_NVIC_SetPriority(LCD_DMA_IRQn, 7, 0);
  HAL_NVIC_EnableIRQ(LCD_DMA_IRQn);
}

void Lcd_FillAsync(uint16_t colour)
{
  Lcd_WaitIdle();

  lcd_fill_colour = colour;
  lcd_start_frame((uint32_t)&lcd_fill_colour, false);
}

void Lcd_BlitAsync(const uint16_t *pixels)
{
  lcd_start_frame((uint32_t)pixels, true);
}

bool Lcd_IsBusy(void)
{
  return lcd_busy;
}

void Lcd_WaitIdle(void)
{
  while (lcd_busy) { }
}

void Lcd_FillRect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                  uint16_t colour)
{
  uint32_t count;

  if ((x >= LCD_WIDTH) || (y >= LCD_HEIGHT) || (w == 0U) || (h == 0U))
  {
    return;
  }

  if ((uint32_t)x + w > LCD_WIDTH)  { w = (uint16_t)(LCD_WIDTH  - x); }
  if ((uint32_t)y + h > LCD_HEIGHT) { h = (uint16_t)(LCD_HEIGHT - y); }

  Lcd_WaitIdle();

  lcd_begin_window(x, y, w, h);

  count = (uint32_t)w * h;

  for (uint32_t i = 0U; i < count; i++)
  {
    spi_write16(colour);
  }

  spi_wait_idle();
  lcd_cs(false);
}

uint32_t Lcd_GetFrameCount(void)
{
  return lcd_frame_count;
}

uint32_t Lcd_GetSpiHz(void)
{
  return HAL_RCC_GetPCLK2Freq() / LCD_SPI_DIV;
}
