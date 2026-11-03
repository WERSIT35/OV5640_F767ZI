/**
  ******************************************************************************
  * @file    sd.c
  * @brief   SD card presence check (SPI mode CMD0) on the shared SPI1 bus.
  ******************************************************************************
  */

#include "sd.h"
#include "main.h"

#define SD_POLL_MS     2000U

/* APB2/256 = 422 kHz: SD cards must be initialised at ~400 kHz or less. */
#define SD_SPI_BR      7U

static bool sd_present;

static uint8_t sd_xfer(uint8_t out)
{
  while ((SPI1->SR & SPI_SR_TXE) == 0U) { }
  *(__IO uint8_t *)&SPI1->DR = out;

  while ((SPI1->SR & SPI_SR_RXNE) == 0U) { }
  return *(__IO uint8_t *)&SPI1->DR;
}

static void sd_cs(bool active)
{
  HAL_GPIO_WritePin(SD_CS_GPIO_Port, SD_CS_Pin,
                    active ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

static bool sd_probe(void)
{
  uint32_t cr1;
  uint8_t  r1 = 0xFFU;

  while ((SPI1->SR & SPI_SR_FTLVL) != 0U) { }
  while ((SPI1->SR & SPI_SR_BSY) != 0U) { }

  cr1 = SPI1->CR1;

  SPI1->CR1 &= ~SPI_CR1_SPE;
  SPI1->CR2  = (SPI1->CR2 & ~SPI_CR2_DS) | (0x7U << SPI_CR2_DS_Pos)
             | SPI_CR2_FRXTH;
  SPI1->CR1  = (cr1 & ~(SPI_CR1_BR | SPI_CR1_SPE))
             | (SD_SPI_BR << SPI_CR1_BR_Pos);
  SPI1->CR1 |= SPI_CR1_SPE;

  /* Drop whatever the LCD traffic left in the receive FIFO. */
  while ((SPI1->SR & SPI_SR_RXNE) != 0U)
  {
    (void)*(__IO uint8_t *)&SPI1->DR;
  }
  (void)SPI1->SR;

  /* At least 74 clocks with CS high to wake the card into SPI mode. */
  sd_cs(false);
  for (uint32_t i = 0U; i < 10U; i++)
  {
    (void)sd_xfer(0xFFU);
  }

  /* CMD0, GO_IDLE_STATE, with its fixed CRC. */
  sd_cs(true);
  (void)sd_xfer(0x40U);
  (void)sd_xfer(0x00U);
  (void)sd_xfer(0x00U);
  (void)sd_xfer(0x00U);
  (void)sd_xfer(0x00U);
  (void)sd_xfer(0x95U);

  /* R1 arrives within 8 bytes; 0xFF means nothing has answered yet. */
  for (uint32_t i = 0U; (i < 10U) && (r1 == 0xFFU); i++)
  {
    r1 = sd_xfer(0xFFU);
  }

  sd_cs(false);

  /* One more byte so the card releases MISO, which the touch controller
   * also uses. */
  (void)sd_xfer(0xFFU);

  while ((SPI1->SR & SPI_SR_FTLVL) != 0U) { }
  while ((SPI1->SR & SPI_SR_BSY) != 0U) { }

  SPI1->CR1 &= ~SPI_CR1_SPE;
  SPI1->CR1  = cr1;

  return r1 == 0x01U;
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

  first      = false;
  last       = now;
  sd_present = sd_probe();
}

bool Sd_IsPresent(void)
{
  return sd_present;
}
