#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>

#define FIRMWARE_VERSION "2.2.1"
#define MOTION_CENTER_MIN_VERSION "1.1"
#ifndef BOX_VERSION
  #define BOX_VERSION 2
#endif
#define PROCESSOR "ATMEGA32U4"
#ifndef MAX_ACTUATORS
  #define MAX_ACTUATORS 7
#endif
#define TARGET_LEONARDO
#include "Actuator.h"
#include "PosMap.h"     // librairie commune : mapping 15 bits + POSMAP_INPUT_MAX
#include "HomingDir.h"  // librairie commune : direction de homing MIN/MAX

#define HOMING_SPS_MIN 50u
#define HOMING_SPS_MAX 10000u


extern unsigned actuatorMax[MAX_ACTUATORS];
extern uint16_t margin[MAX_ACTUATORS];
extern unsigned max[MAX_ACTUATORS];

// ── Shims position/target ────────────────────────────────────────────────────
// mPosition[i] et mTarget[i] routent vers ax[i].pos / ax[i].target.
// Compatibilité syntaxique totale pendant la migration vers les structs.
struct PosProxy {
    volatile uint16_t& operator[](uint8_t i) const { return ax[i].pos; }
};
struct TgtProxy {
    volatile uint16_t& operator[](uint8_t i) const { return ax[i].target; }
};
extern PosProxy mPosition;
extern TgtProxy mTarget;

// ── Accès 16 bits atomiques ──────────────────────────────────────────────────────────
// Le stepping est en bit-bang depuis loop() (plus d'ISR moteur), mais ces
// helpers restent : cout negligeable et ils protegent contre toute future
// reintroduction d'un contexte concurrent (ISR/USB) touchant pos/target.
#include <util/atomic.h>
static inline void atomicSetTarget(uint8_t i, uint16_t v) {
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { mTarget[i] = v; }
}
static inline uint16_t atomicGetPosition(uint8_t i) {
  uint16_t v;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { v = mPosition[i]; }
  return v;
}
// Gel d'un axe : target <- position (cohérent même si l'ISR vient de bouger).
static inline void atomicHoldAxis(uint8_t i) {
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { mTarget[i] = mPosition[i]; }
}
// Test "axe à la cible" sur un instantané cohérent pos/target.
static inline bool atomicAxisAtTarget(uint8_t i) {
  uint16_t p, t;
  ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { p = mPosition[i]; t = mTarget[i]; }
  return p == t;
}

// Arrays tracking whether each axis is connected and whether it is currently
// homing/calibrating.  Motors 1‑4 are assumed to be present; entries 4 and 5
// (indices 4 and 5) correspond to the optional traction loss and extra axes.
// These are defined in Globals.cpp and initialised to `false` for all axes.
extern bool mConnected[MAX_ACTUATORS];
extern bool calibrated[MAX_ACTUATORS];
extern bool calibrating[MAX_ACTUATORS];
extern bool homed[MAX_ACTUATORS];
extern bool hostConnected;  // true when Python UI or SimHub has an active session
extern bool mcConnected;  // Motion Center session state (MC_CONNECTED)
extern bool simhubConnected;  // true when session was opened by SimHub handshake
enum SimHubState : uint8_t {
  SH_STATE_NONE = 0,
  SH_STATE_IDLE,
  SH_STATE_MOTION_READY,
  SH_STATE_MOTION_ACTIVE
};
extern SimHubState simhubState;
extern unsigned long lastApiTime;  // timestamp of last host API command (millis)
extern uint8_t expectedBytesP;  // nb d’octets attendus pour 'P'
extern uint8_t activeMotorsCount;

// Recompute hostConnected from mcConnected/simhubConnected.
void syncHostConnectionState();
// Apply connection LED policy (servo-off only):
// none or SimHub idle -> green, MC -> blue.
void refreshConnectionLed();
// -----------------------------------------------------------------------------
// EEPROM offsets for persisting configuration

void FactoryReset(bool m5Connected, bool m6Connected);

void recomputeExpectedBytesP();
#define SecurityPin 2

extern int8_t actuatorDir[MAX_ACTUATORS];

// EEPROM schema version stored at EEPROM_HAS_BEEN_FACTORYRESET.
// Increment this value whenever the EEPROM layout changes so the board
// automatically wipes stale data on the first boot after a firmware update.
// v3: max M1..M4 en unites 15 bits (defaut 32767, ancien defaut 65535).
#define EEPROM_SCHEMA_VERSION 3

/* -------------------- TABLE D'ADRESSES AVR (EEPROM) -------------------- */
#if defined(__AVR_ATmega32U4__)
/* Offsets en OCTETS dans l'EEPROM AVR */
#define EEPROM_M1MAX_ADDR 0
#define EEPROM_M1MARGIN_ADDR 4
#define EEPROM_M2MAX_ADDR 8
#define EEPROM_M2MARGIN_ADDR 12
#define EEPROM_M3MAX_ADDR 16
#define EEPROM_M3MARGIN_ADDR 20
#define EEPROM_M4MAX_ADDR 24
#define EEPROM_M4MARGIN_ADDR 28
#define EEPROM_M5MAX_ADDR 32
#define EEPROM_M5MARGIN_ADDR 36
#define EEPROM_M6MAX_ADDR 40
#define EEPROM_M6MARGIN_ADDR 44

#define EEPROM_M1CONNECTED_ADDR 48
#define EEPROM_M2CONNECTED_ADDR 52
#define EEPROM_M3CONNECTED_ADDR 56
#define EEPROM_M4CONNECTED_ADDR 60
#define EEPROM_M5CONNECTED_ADDR 64
#define EEPROM_M6CONNECTED_ADDR 68

#define EEPROM_HAS_BEEN_FACTORYRESET 72
/* Direction de homing (0=MIN, 1=MAX — shared/HomingDir.h). Lecture avec
   garde "== 1" : une cellule vierge (0xFFFF) retombe sur MIN. */
#define EEPROM_M5HOMINGDIR_ADDR 76
#define EEPROM_M6HOMINGDIR_ADDR 80
/* Endpark % (0..100, 255=off). Cellule vierge (0xFFFF) => off (garde <=100). */
#define EEPROM_M5ENDPARK_ADDR 84
#define EEPROM_M6ENDPARK_ADDR 88
#define EEPROM_HOMINGSPS_ADDR 0x0120u /* 288 */

/* Helpers lecture/écriture AVR – sans “librairie” en plus :
   - si ARDUINO: utilise le core EEPROM
   - sinon: utilise <avr/eeprom.h> (libc AVR standard) */
#ifdef ARDUINO
#include <EEPROM.h>
static inline uint32_t nvm_read_u32(int a) {
  uint32_t v;
  EEPROM.get(a, v);
  return v;
}
static inline void nvm_write_u32(int a, uint32_t v) {
  EEPROM.put(a, v);
}
static inline uint16_t nvm_read_u16(int a) {
  uint16_t v;
  EEPROM.get(a, v);
  return v;
}
static inline void nvm_write_u16(int a, uint16_t v) {
  EEPROM.put(a, v);
}
static inline bool nvm_read_bool32(int a) {
  uint32_t v;
  EEPROM.get(a, v);
  return v != 0;
}
static inline void nvm_write_bool32(int a, bool b) {
  uint32_t v = b ? 1u : 0u;
  EEPROM.put(a, v);
}
#else
#include <avr/eeprom.h>
static inline uint32_t nvm_read_u32(int a) {
  uint32_t v;
  eeprom_read_block(&v, (const void*)a, 4);
  return v;
}
static inline void nvm_write_u32(int a, uint32_t v) {
  eeprom_write_block(&v, (void*)a, 4);
}
static inline uint16_t nvm_read_u16(int a) {
  uint16_t v;
  eeprom_read_block(&v, (const void*)a, 2);
  return v;
}
static inline void nvm_write_u16(int a, uint16_t v) {
  eeprom_write_block(&v, (void*)a, 2);
}
static inline bool nvm_read_bool32(int a) {
  uint32_t v;
  eeprom_read_block(&v, (const void*)a, 4);
  return v != 0;
}
static inline void nvm_write_bool32(int a, bool b) {
  uint32_t v = b ? 1u : 0u;
  eeprom_write_block(&v, (void*)a, 4);
}
#endif


/* -------------------- TABLE D'ADRESSES STM32F103 (Flash 1 KB) -------------------- */
#else
/* Base = dernière page 1KB: 0x0800FC00–0x0800FFFF */
#ifndef NVM_BASE_ADDR
#define NVM_BASE_ADDR 0x0800FC00UL
#endif
#define NVM_PAGE_SIZE 1024u
#define NVM_END_ADDR (NVM_BASE_ADDR + NVM_PAGE_SIZE)
#define NVM_ABS(off) ((uint32_t)(NVM_BASE_ADDR + (uint32_t)(off)))

/* Offsets en OCTETS (identiques à l’AVR pour rester simple) */
#define EEPROM_M1MAX_ADDR 0
#define EEPROM_M1MARGIN_ADDR 4
#define EEPROM_M2MAX_ADDR 8
#define EEPROM_M2MARGIN_ADDR 12
#define EEPROM_M3MAX_ADDR 16
#define EEPROM_M3MARGIN_ADDR 20
#define EEPROM_M4MAX_ADDR 24
#define EEPROM_M4MARGIN_ADDR 28
#define EEPROM_M5MAX_ADDR 32
#define EEPROM_M5MARGIN_ADDR 36
#define EEPROM_M6MAX_ADDR 40
#define EEPROM_M6MARGIN_ADDR 44

#define EEPROM_M1CONNECTED_ADDR 48
#define EEPROM_M2CONNECTED_ADDR 52
#define EEPROM_M3CONNECTED_ADDR 56
#define EEPROM_M4CONNECTED_ADDR 60
#define EEPROM_M5CONNECTED_ADDR 64
#define EEPROM_M6CONNECTED_ADDR 68

#define EEPROM_HAS_BEEN_FACTORYRESET 72
#define EEPROM_M5HOMINGDIR_ADDR 76
#define EEPROM_M6HOMINGDIR_ADDR 80
#define EEPROM_M5ENDPARK_ADDR 84
#define EEPROM_M6ENDPARK_ADDR 88
#define EEPROM_HOMINGSPS_ADDR 0x0120u /* 288 */

/* Garde-fou : tout tient dans 1 page */
#define NVM_LAST_USED (EEPROM_HOMINGSPS_ADDR + 4u)
#if (NVM_LAST_USED > NVM_PAGE_SIZE)
#error "Table NVM > 1KB (dernière page F103C8)."
#endif

/* Implé HAL minimaliste (aucune lib en plus) */
#include "stm32f1xx_hal.h"
#include <string.h>

static inline bool flash_erase_page(uint32_t page_addr) {
  HAL_FLASH_Unlock();
  FLASH_EraseInitTypeDef e = { .TypeErase = FLASH_TYPEERASE_PAGES, .PageAddress = page_addr, .NbPages = 1 };
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
    if (i + 1 < len) half |= (uint16_t)buf[i + 1] << 8;
    st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, page_addr + i, half);
    if (st != HAL_OK) break;
  }
  HAL_FLASH_Lock();
  return (st == HAL_OK);
}

/* Lectures brutes */
static inline bool NVM_ReadU32(uint32_t off, uint32_t *v) {
  if (!v || (NVM_ABS(off) + 4u) > NVM_END_ADDR) return false;
  *v = *(__IO uint32_t *)NVM_ABS(off);
  return true;
}
static inline bool NVM_ReadU16(uint32_t off, uint16_t *v) {
  if (!v || (NVM_ABS(off) + 2u) > NVM_END_ADDR) return false;
  *v = *(__IO uint16_t *)NVM_ABS(off);
  return true;
}
static inline bool NVM_ReadU8(uint32_t off, uint8_t *v) {
  if (!v || (NVM_ABS(off) + 1u) > NVM_END_ADDR) return false;
  *v = *(__IO uint8_t *)NVM_ABS(off);
  return true;
}

/* Écritures sûres (snapshot de la page → erase → reprogram page) */
static inline bool nvm_write_update(uint32_t off, const void *src, uint32_t size) {
  if ((NVM_ABS(off) + size) > NVM_END_ADDR) return false;
  uint8_t page[NVM_PAGE_SIZE];
  memcpy(page, (const void *)NVM_BASE_ADDR, NVM_PAGE_SIZE);
  memcpy(&page[off], src, size);
  __disable_irq();
  bool ok = flash_erase_page(NVM_BASE_ADDR);
  if (ok) ok = flash_program_page(NVM_BASE_ADDR, page, NVM_PAGE_SIZE);
  __enable_irq();
  return ok;
}
static inline uint32_t nvm_read_u32(uint32_t off) {
  uint32_t v = 0;
  (void)NVM_ReadU32(off, &v);
  return v;
}
static inline void nvm_write_u32(uint32_t off, uint32_t v) {
  (void)nvm_write_update(off, &v, sizeof(v));
}
static inline uint16_t nvm_read_u16(uint32_t off) {
  uint16_t v = 0;
  (void)NVM_ReadU16(off, &v);
  return v;
}
static inline void nvm_write_u16(uint32_t off, uint16_t v) {
  (void)nvm_write_update(off, &v, sizeof(v));
}
static inline bool nvm_read_bool32(uint32_t off) {
  uint32_t v = 0;
  (void)NVM_ReadU32(off, &v);
  return v != 0u;
}
static inline void nvm_write_bool32(uint32_t off, bool b) {
  uint32_t v = b ? 1u : 0u;
  (void)nvm_write_update(off, &v, sizeof(v));
}

#endif /* TARGET_STM32F103 */
/* ================== /NVM single-file ================== */


/*
#define EEPROM_M1MAX_ADDR         0
#define EEPROM_M1MARGIN_ADDR      4
#define EEPROM_M2MAX_ADDR         8
#define EEPROM_M2MARGIN_ADDR     12
#define EEPROM_M3MAX_ADDR        16
#define EEPROM_M3MARGIN_ADDR     20
#define EEPROM_M4MAX_ADDR        24
#define EEPROM_M4MARGIN_ADDR     28
#define EEPROM_M5MAX_ADDR        32
#define EEPROM_M5MARGIN_ADDR     36
#define EEPROM_M6MAX_ADDR        40
#define EEPROM_M6MARGIN_ADDR     44

#define EEPROM_M1CONNECTED_ADDR  48
#define EEPROM_M2CONNECTED_ADDR  52
#define EEPROM_M3CONNECTED_ADDR  56
#define EEPROM_M4CONNECTED_ADDR  60
#define EEPROM_M5CONNECTED_ADDR  64
#define EEPROM_M6CONNECTED_ADDR  68
#define EEPROM_HOMINGSPS_ADDR  0x0120 

#define EEPROM_HAS_BEEN_FACTORYRESET  72




// End of Globals.h*/
