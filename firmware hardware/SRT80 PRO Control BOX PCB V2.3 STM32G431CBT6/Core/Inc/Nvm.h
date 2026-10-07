#pragma once
// ================================================================
//  Nvm.h — persistance NVM STM32G431 (derniere page 2 KB du flash)
//
//  Portage de la version F103 (halfword) vers le G4 :
//   - programmation flash en DOUBLEWORD (64 bits) alignee sur 8 octets ;
//   - page de 2 KB ; G431 MONO-BANQUE -> erase sur FLASH_BANK_1 ;
//   - schema etendu aux 6 moteurs (max/margin/connected/homingdir/endpark).
//
//  Interface hwEeprom* identique a l'AVR / F103 : lecture directe (flash
//  memory-mapped), ecriture par snapshot page -> erase -> reprogram.
//  ATTENTION : ecriture ~20-40 ms IRQ coupees -> uniquement servos a l'arret.
// ================================================================
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "stm32g4xx_hal.h"

/* Base = derniere page 2KB du flash 128 KB (0x0801F800..0x0801FFFF).
   0x0801F800 = page 63 (mono-banque). Le linker exclut cette page (126 KB). */
#ifndef NVM_BASE_ADDR
#define NVM_BASE_ADDR   0x0801F800UL
#endif
#define NVM_PAGE_SIZE   2048u
#define NVM_BANK        FLASH_BANK_1
#define NVM_PAGE_INDEX  63u                       /* (0x0801F800-0x08000000)/2048 */
#define NVM_END_ADDR    (NVM_BASE_ADDR + NVM_PAGE_SIZE)
#define NVM_ABS(off)    ((uint32_t)(NVM_BASE_ADDR + (uint32_t)(off)))

/* ---- Schema V2.3 (6 moteurs), offsets en OCTETS, champs u16 ------------- */
/* MAX / MARGIN : M1..M6 */
#define EEPROM_M1MAX_ADDR      0
#define EEPROM_M2MAX_ADDR      4
#define EEPROM_M3MAX_ADDR      8
#define EEPROM_M4MAX_ADDR     12
#define EEPROM_M5MAX_ADDR     16
#define EEPROM_M6MAX_ADDR     20
#define EEPROM_M1MARGIN_ADDR  24
#define EEPROM_M2MARGIN_ADDR  28
#define EEPROM_M3MARGIN_ADDR  32
#define EEPROM_M4MARGIN_ADDR  36
#define EEPROM_M5MARGIN_ADDR  40
#define EEPROM_M6MARGIN_ADDR  44
/* CONNECTED : M1..M6 */
#define EEPROM_M1CONNECTED_ADDR 48
#define EEPROM_M2CONNECTED_ADDR 50
#define EEPROM_M3CONNECTED_ADDR 52
#define EEPROM_M4CONNECTED_ADDR 54
#define EEPROM_M5CONNECTED_ADDR 56
#define EEPROM_M6CONNECTED_ADDR 58
/* HOMING_DIR : M1..M6 (0=MIN, 1=MAX — shared/HomingDir.h) */
#define EEPROM_M1HOMINGDIR_ADDR 60
#define EEPROM_M2HOMINGDIR_ADDR 62
#define EEPROM_M3HOMINGDIR_ADDR 64
#define EEPROM_M4HOMINGDIR_ADDR 66
#define EEPROM_M5HOMINGDIR_ADDR 68
#define EEPROM_M6HOMINGDIR_ADDR 70
/* ENDPARK % (0..100, 255=off) : M1..M6 */
#define EEPROM_M1ENDPARK_ADDR   72
#define EEPROM_M2ENDPARK_ADDR   74
#define EEPROM_M3ENDPARK_ADDR   76
#define EEPROM_M4ENDPARK_ADDR   78
#define EEPROM_M5ENDPARK_ADDR   80
#define EEPROM_M6ENDPARK_ADDR   82

#define EEPROM_HAS_BEEN_FACTORYRESET 84
#define EEPROM_HOMINGSPS_ADDR        88

/* Schema de cette box (nouveau layout 6 moteurs). */
#ifndef EEPROM_SCHEMA_VERSION
#define EEPROM_SCHEMA_VERSION 3
#endif

/* Garde-fou : tout tient dans 1 page */
#define NVM_LAST_USED (EEPROM_HOMINGSPS_ADDR + 4u)
#if (NVM_LAST_USED > NVM_PAGE_SIZE)
#error "Table NVM > 2KB (derniere page banque 2 G431)."
#endif

static inline bool nvm_erase_page(void) {
  FLASH_EraseInitTypeDef e = {0};
  e.TypeErase = FLASH_TYPEERASE_PAGES;
  e.Banks     = NVM_BANK;
  e.Page      = NVM_PAGE_INDEX;
  e.NbPages   = 1u;
  uint32_t err = 0;
  return (HAL_FLASHEx_Erase(&e, &err) == HAL_OK);
}

/* Programmation doubleword (64 bits) : len doit etre multiple de 8 (page
   shadow de 2KB -> toujours vrai). Octets non ecrits laisses a 0xFF. */
static inline bool nvm_program_page(const uint8_t *buf, uint32_t len) {
  if (len > NVM_PAGE_SIZE) return false;
  HAL_StatusTypeDef st = HAL_OK;
  for (uint32_t i = 0; i < len; i += 8) {
    uint64_t dw = 0xFFFFFFFFFFFFFFFFULL;
    memcpy(&dw, &buf[i], (len - i >= 8u) ? 8u : (len - i));
    st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, NVM_BASE_ADDR + i, dw);
    if (st != HAL_OK) break;
  }
  return (st == HAL_OK);
}

/* Ecriture sure : snapshot page -> erase -> reprogram.
   Buffer STATIQUE (2KB) : jamais sur la pile. Non reentrant : boucle
   principale uniquement, jamais depuis une ISR. */
static inline bool nvm_write_update(uint32_t off, const void *src, uint32_t size) {
  if ((NVM_ABS(off) + size) > NVM_END_ADDR) return false;
  static uint8_t page[NVM_PAGE_SIZE];
  memcpy(page, (const void *)NVM_BASE_ADDR, NVM_PAGE_SIZE);
  memcpy(&page[off], src, size);
  __disable_irq();
  HAL_FLASH_Unlock();
  bool ok = nvm_erase_page();
  if (ok) ok = nvm_program_page(page, NVM_PAGE_SIZE);
  HAL_FLASH_Lock();
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

/* Compat noms hwEeprom* (memes signatures que HardwareAbstraction AVR) */
static inline uint16_t hwEepromReadU16(uint16_t addr)             { return nvm_read_u16(addr); }
static inline void     hwEepromWriteU16(uint16_t addr, uint16_t v){ nvm_write_u16(addr, v); }
