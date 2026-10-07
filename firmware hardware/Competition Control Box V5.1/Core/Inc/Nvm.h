#pragma once
// ================================================================
//  Nvm.h — persistance NVM STM32F103 (dernière page flash 1 KB)
//
//  Extraction C pur de la section STM32 de Globals.h (qui contient du
//  C++ en tête et ne peut pas être inclus depuis main.c). Même table
//  d'adresses, même schéma : un futur passage par Globals.h restera
//  compatible binaire.
// ================================================================
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "stm32f1xx_hal.h"

/* Base = dernière page 1KB du F103C8 : 0x0800FC00–0x0800FFFF */
#ifndef NVM_BASE_ADDR
#define NVM_BASE_ADDR 0x0800FC00UL
#endif
#define NVM_PAGE_SIZE 1024u
#define NVM_END_ADDR  (NVM_BASE_ADDR + NVM_PAGE_SIZE)
#define NVM_ABS(off)  ((uint32_t)(NVM_BASE_ADDR + (uint32_t)(off)))

/* Offsets en OCTETS (identiques à l'AVR — voir Globals.h) */
#define EEPROM_M1MAX_ADDR     0
#define EEPROM_M1MARGIN_ADDR  4
#define EEPROM_M2MAX_ADDR     8
#define EEPROM_M2MARGIN_ADDR 12
#define EEPROM_M3MAX_ADDR    16
#define EEPROM_M3MARGIN_ADDR 20
#define EEPROM_M4MAX_ADDR    24
#define EEPROM_M4MARGIN_ADDR 28
#define EEPROM_M5MAX_ADDR    32
#define EEPROM_M5MARGIN_ADDR 36
#define EEPROM_M6MAX_ADDR    40
#define EEPROM_M6MARGIN_ADDR 44

#define EEPROM_M1CONNECTED_ADDR 48
#define EEPROM_M2CONNECTED_ADDR 52
#define EEPROM_M3CONNECTED_ADDR 56
#define EEPROM_M4CONNECTED_ADDR 60
#define EEPROM_M5CONNECTED_ADDR 64
#define EEPROM_M6CONNECTED_ADDR 68

#define EEPROM_HAS_BEEN_FACTORYRESET 72
#define EEPROM_HOMINGSPS_ADDR 0x0120u /* 288 */

/* Schéma : doit matcher EEPROM_SCHEMA_VERSION de Globals.h (AVR) */
#ifndef EEPROM_SCHEMA_VERSION
#define EEPROM_SCHEMA_VERSION 2
#endif

/* Garde-fou : tout tient dans 1 page */
#define NVM_LAST_USED (EEPROM_HOMINGSPS_ADDR + 4u)
#if (NVM_LAST_USED > NVM_PAGE_SIZE)
#error "Table NVM > 1KB (dernière page F103C8)."
#endif

static inline bool flash_erase_page(uint32_t page_addr) {
  HAL_FLASH_Unlock();
  FLASH_EraseInitTypeDef e;
  e.TypeErase   = FLASH_TYPEERASE_PAGES;
  e.PageAddress = page_addr;
  e.NbPages     = 1;
  uint32_t err = 0;
  HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&e, &err);
  HAL_FLASH_Lock();
  return (st == HAL_OK);
}

static inline bool flash_program_page(uint32_t page_addr, const uint8_t *buf, uint32_t len) {
  if (len > NVM_PAGE_SIZE) return false;
  HAL_FLASH_Unlock();
  HAL_StatusTypeDef st = HAL_OK;
  for (uint32_t i = 0; i < len; i += 2) {
    uint16_t half = buf[i];
    if (i + 1 < len) half |= (uint16_t)((uint16_t)buf[i + 1] << 8);
    st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, page_addr + i, half);
    if (st != HAL_OK) break;
  }
  HAL_FLASH_Lock();
  return (st == HAL_OK);
}

/* Écriture sûre : snapshot de la page → erase → reprogram.
   ATTENTION : ~20-40 ms avec IRQ coupées (erase+program). À n'appeler que
   servos à l'arrêt (fin de calibration, SET) — jamais pendant le motion. */
static inline bool nvm_write_update(uint32_t off, const void *src, uint32_t size) {
  if ((NVM_ABS(off) + size) > NVM_END_ADDR) return false;
  /* CRITIQUE : buffer en statique, PAS sur la pile. 1 KB de local dépasserait
     la pile CubeMX par défaut du F103 (_Min_Stack_Size 0x400) -> stack
     overflow -> corruption memoire. Non reentrant : a n'appeler que depuis
     la boucle principale (jamais depuis une ISR). */
  static uint8_t page[NVM_PAGE_SIZE];
  memcpy(page, (const void *)NVM_BASE_ADDR, NVM_PAGE_SIZE);
  memcpy(&page[off], src, size);
  __disable_irq();
  bool ok = flash_erase_page(NVM_BASE_ADDR);
  if (ok) ok = flash_program_page(NVM_BASE_ADDR, page, NVM_PAGE_SIZE);
  __enable_irq();
  return ok;
}

static inline uint16_t nvm_read_u16(uint32_t off) {
  if ((NVM_ABS(off) + 2u) > NVM_END_ADDR) return 0;
  return *(__IO uint16_t *)NVM_ABS(off);
}
static inline void nvm_write_u16(uint32_t off, uint16_t v) {
  (void)nvm_write_update(off, &v, sizeof(v));
}

/* Compat noms hwEeprom* (mêmes signatures que HardwareAbstraction AVR) */
static inline uint16_t hwEepromReadU16(uint16_t addr)            { return nvm_read_u16(addr); }
static inline void     hwEepromWriteU16(uint16_t addr, uint16_t v){ nvm_write_u16(addr, v); }
