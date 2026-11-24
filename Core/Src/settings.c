/**
  ******************************************************************************
  * @file    settings.c
  * @brief   User settings as an append-only log in flash sector 10.
  *
  * Each save takes the next free 32-byte slot, so the 256K sector (next to
  * the touch calibration in sector 11) is erased only once every 8192 saves.
  ******************************************************************************
  */

#include "settings.h"
#include "main.h"
#include <string.h>

#define SET_ADDR     0x08180000UL
#define SET_SECTOR   FLASH_SECTOR_10
#define SET_SLOTS    8192U
#define SET_MAGIC    0x53455431UL        /* "SET1" */
#define SET_EMPTY    0xFFFFFFFFUL

typedef struct
{
  uint32_t   magic;
  Settings_t data;
  uint32_t   check;
  uint32_t   pad[2];
} Slot_t;

_Static_assert(sizeof(Slot_t) == 32U, "slot size");

static uint32_t next_slot;

static const Slot_t *slot_at(uint32_t i)
{
  return (const Slot_t *)(SET_ADDR + (i * sizeof(Slot_t)));
}

static uint32_t checksum(const Settings_t *s)
{
  const uint8_t *p   = (const uint8_t *)s;
  uint32_t       sum = 0xA5A5A5A5UL;

  for (uint32_t i = 0U; i < sizeof(*s); i++)
  {
    sum = ((sum << 5) | (sum >> 27)) ^ p[i];
  }

  return sum;
}

bool Settings_Load(Settings_t *s)
{
  bool found = false;

  for (next_slot = 0U; next_slot < SET_SLOTS; next_slot++)
  {
    const Slot_t *slot = slot_at(next_slot);

    if (slot->magic == SET_EMPTY)
    {
      break;
    }

    if ((slot->magic == SET_MAGIC) && (slot->check == checksum(&slot->data)))
    {
      *s    = slot->data;
      found = true;
    }
  }

  return found;
}

void Settings_Save(const Settings_t *s)
{
  Slot_t          slot;
  const uint32_t *w = (const uint32_t *)&slot;

  if ((next_slot > 0U) &&
      (memcmp(&slot_at(next_slot - 1U)->data, s, sizeof(*s)) == 0))
  {
    return;
  }

  memset(&slot, 0, sizeof(slot));
  slot.magic = SET_MAGIC;
  slot.data  = *s;
  slot.check = checksum(s);

  HAL_FLASH_Unlock();

  if (next_slot >= SET_SLOTS)
  {
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t               error = 0U;

    erase.TypeErase    = FLASH_TYPEERASE_SECTORS;
    erase.Sector       = SET_SECTOR;
    erase.NbSectors    = 1U;
    erase.VoltageRange = FLASH_VOLTAGE_RANGE_3;
    (void)HAL_FLASHEx_Erase(&erase, &error);
    next_slot = 0U;
  }

  for (uint32_t i = 0U; i < (sizeof(slot) / 4U); i++)
  {
    (void)HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                            (uint32_t)slot_at(next_slot) + (i * 4U), w[i]);
  }

  HAL_FLASH_Lock();

  SCB_InvalidateDCache_by_Addr((uint32_t *)slot_at(next_slot), 32);
  next_slot++;
}
