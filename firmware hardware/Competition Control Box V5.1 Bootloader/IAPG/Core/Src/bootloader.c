/**
 ******************************************************************************
 * @file    bootloader.c
 * @brief   Bootloader USB-CDC resident pour STM32G474VET6 — implementation.
 ******************************************************************************
 */
#include "bootloader.h"
#include "main.h"
#include <string.h>

#include "usb_device.h"        /* hUsbDeviceFS                               */
#include "usbd_cdc.h"          /* USBD_CDC_HandleTypeDef                     */
#include "usbd_cdc_if.h"       /* CDC_Transmit_FS                            */

extern USBD_HandleTypeDef hUsbDeviceFS;

/* ------------------------------------------------------------------------- */
/*  Configuration                                                            */
/* ------------------------------------------------------------------------- */
#define BL_VERSION        1u

/* L'application occupe toute la BANQUE 2. Le bootloader (banque 1) la programme
   sans blocage (read-while-write inter-banque du G4). */
#define APP_ADDR          0x08040000UL          /* doit == STM32_APP_ADDR app */
#define FLASH_START       0x08000000UL
#define FLASH_END         0x08080000UL          /* 512 Ko                     */
#define PAGE_SIZE         0x800UL               /* 2 Ko (G474 dual-bank)      */
#define NUM_APP_PAGES     ((FLASH_END - APP_ADDR) / PAGE_SIZE)     /* = 128   */

/* Magic d'entree DFU : DOIT correspondre a MC_DFU_MAGIC de l'application. */
#define BL_DFU_MAGIC      0x00004F50UL

#define W_DATA_MAX        1024u

/* ------------------------------------------------------------------------- */
/*  Ring buffer USB (rempli en IT, vide en boucle principale)                */
/* ------------------------------------------------------------------------- */
#define RING_SIZE   2048u                       /* puissance de 2             */
#define RING_MASK   (RING_SIZE - 1u)
static volatile uint8_t  s_ring[RING_SIZE];
static volatile uint16_t s_head;
static volatile uint16_t s_tail;

/* Bitmap des pages deja effacees durant la session (1 bit / page). */
static uint8_t s_erased[(NUM_APP_PAGES + 7u) / 8u];

/* ------------------------------------------------------------------------- */
/*  Parser                                                                   */
/* ------------------------------------------------------------------------- */
typedef enum { S_CMD, S_W_HDR, S_W_DATA, S_W_CRC, S_G } pstate_t;
static pstate_t s_state;
static uint8_t  s_hdr[8];
static uint8_t  s_hn;
static uint8_t  s_data[W_DATA_MAX];
static uint16_t s_dlen;
static uint16_t s_dn;
static uint32_t s_waddr;

/* ------------------------------------------------------------------------- */
/*  Helpers USB TX                                                           */
/* ------------------------------------------------------------------------- */
static void bl_send(const uint8_t *p, uint16_t n)
{
  static uint8_t txbuf[4];
  if (n > sizeof(txbuf)) { n = sizeof(txbuf); }
  USBD_CDC_HandleTypeDef *hcdc =
      (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;
  if (hcdc == NULL) { return; }

  for (uint32_t k = 0; (k < 200000u) && (hcdc->TxState != 0u); k++) { __NOP(); }
  if (hcdc->TxState != 0u) { return; }

  memcpy(txbuf, p, n);
  for (uint16_t k = 0; k < 64u; k++) {
    if (CDC_Transmit_FS(txbuf, n) == USBD_OK) { return; }
    for (uint32_t d = 0; d < 2000u; d++) { __NOP(); }
  }
}
static void ack(uint8_t c) { bl_send(&c, 1u); }

/* ------------------------------------------------------------------------- */
/*  CRC logicielles (memes definitions que cote PC)                          */
/* ------------------------------------------------------------------------- */
static uint16_t crc16_ccitt(const uint8_t *d, uint16_t len)   /* == crc_hqx  */
{
  uint16_t crc = 0xFFFFu;
  for (uint16_t i = 0; i < len; i++) {
    crc ^= (uint16_t)((uint16_t)d[i] << 8);
    for (uint8_t b = 0; b < 8u; b++) {
      crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                            : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

static uint32_t crc32_zlib(const uint8_t *d, uint32_t len)
{
  uint32_t crc = 0xFFFFFFFFUL;
  for (uint32_t i = 0; i < len; i++) {
    crc ^= d[i];
    for (uint8_t b = 0; b < 8u; b++) {
      crc = (crc >> 1) ^ (0xEDB88420UL & (uint32_t)(-(int32_t)(crc & 1u)));
    }
  }
  return ~crc;
}

/* ------------------------------------------------------------------------- */
/*  Flash                                                                     */
/* ------------------------------------------------------------------------- */
static int erase_page_at(uint32_t addr)
{
  /* L'app est entierement en banque 2 : page = index dans la banque 2. */
  FLASH_EraseInitTypeDef e;
  e.TypeErase = FLASH_TYPEERASE_PAGES;
  e.Banks     = FLASH_BANK_2;
  e.Page      = (addr - APP_ADDR) / PAGE_SIZE;
  e.NbPages   = 1u;
  uint32_t err = 0u;
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
  return (HAL_FLASHEx_Erase(&e, &err) == HAL_OK) ? 0 : -1;
}

/* Efface (une seule fois par session) la/les page(s) couvrant [addr,addr+len). */
static int ensure_erased(uint32_t addr, uint16_t len)
{
  uint32_t a   = addr;
  uint32_t end = addr + len;
  while (a < end) {
    uint32_t idx = (a - APP_ADDR) / PAGE_SIZE;
    if (idx < NUM_APP_PAGES) {
      uint8_t mask = (uint8_t)(1u << (idx & 7u));
      if ((s_erased[idx >> 3] & mask) == 0u) {
        if (erase_page_at(a & ~(PAGE_SIZE - 1u)) != 0) { return -1; }
        s_erased[idx >> 3] |= mask;
      }
    }
    a = (a & ~(PAGE_SIZE - 1u)) + PAGE_SIZE;
  }
  return 0;
}

/* Programme par double-mot (G4) ; complete a 0xFF le dernier mot partiel. */
static int program_block(uint32_t addr, const uint8_t *data, uint16_t len)
{
  for (uint16_t i = 0; i < len; i += 8u) {
    uint64_t dw = 0xFFFFFFFFFFFFFFFFULL;
    uint8_t  n  = (uint16_t)(len - i) >= 8u ? 8u : (uint8_t)(len - i);
    for (uint8_t k = 0; k < n; k++) {
      dw &= ~((uint64_t)0xFFu << (8u * k));
      dw |=  ((uint64_t)data[i + k] << (8u * k));
    }
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr + i, dw) != HAL_OK) {
      return -1;
    }
  }
  return 0;
}

/* ------------------------------------------------------------------------- */
/*  Saut application                                                         */
/* ------------------------------------------------------------------------- */
static int app_valid(void)
{
  uint32_t sp    = *(volatile uint32_t *)APP_ADDR;
  uint32_t entry = *(volatile uint32_t *)(APP_ADDR + 4u);
  if ((sp < 0x20000000UL) || (sp > 0x20020000UL)) { return 0; }   /* SP in SRAM */
  if ((entry < APP_ADDR) || (entry >= FLASH_END))  { return 0; }
  if ((entry & 1u) == 0u)                          { return 0; }   /* Thumb      */
  return 1;
}

static void jump_to_app(void)
{
  uint32_t sp    = *(volatile uint32_t *)APP_ADDR;
  uint32_t entry = *(volatile uint32_t *)(APP_ADDR + 4u);

  __disable_irq();
  SysTick->CTRL = 0u;
  SysTick->LOAD = 0u;
  SysTick->VAL  = 0u;
  SCB->VTOR = APP_ADDR;
  __DSB();
  __ISB();
  __set_MSP(sp);
  __enable_irq();
  ((void (*)(void))entry)();
  for (;;) { }                       /* ne revient pas                        */
}

/* ------------------------------------------------------------------------- */
/*  Parser : 1 octet                                                         */
/* ------------------------------------------------------------------------- */
static void feed(uint8_t b)
{
  switch (s_state) {
    case S_CMD:
      if (b == 'P') {
        uint8_t r[2] = { 'p', (uint8_t)BL_VERSION };
        bl_send(r, 2u);
      } else if (b == 'E') {
        memset(s_erased, 0, sizeof(s_erased));   /* arme : rien encore efface */
        ack('e');
      } else if (b == 'W') {
        s_hn = 0u; s_state = S_W_HDR;
      } else if (b == 'G') {
        s_hn = 0u; s_state = S_G;
      }
      /* sinon : octet ignore (resync, ex. "!DFU\n") */
      break;

    case S_W_HDR:
      s_hdr[s_hn++] = b;
      if (s_hn == 6u) {
        s_waddr = (uint32_t)s_hdr[0] | ((uint32_t)s_hdr[1] << 8)
                | ((uint32_t)s_hdr[2] << 16) | ((uint32_t)s_hdr[3] << 24);
        s_dlen  = (uint16_t)((uint16_t)s_hdr[4] | ((uint16_t)s_hdr[5] << 8));
        if (s_dlen > W_DATA_MAX) { ack('!'); s_state = S_CMD; break; }
        s_dn = 0u;
        s_state = (s_dlen == 0u) ? S_W_CRC : S_W_DATA;
        s_hn = 0u;
      }
      break;

    case S_W_DATA:
      s_data[s_dn++] = b;
      if (s_dn == s_dlen) { s_hn = 0u; s_state = S_W_CRC; }
      break;

    case S_W_CRC:
      s_hdr[s_hn++] = b;
      if (s_hn == 2u) {
        uint16_t want = (uint16_t)((uint16_t)s_hdr[0] | ((uint16_t)s_hdr[1] << 8));
        int ok = 0;
        if (crc16_ccitt(s_data, s_dlen) == want) {
          if (s_waddr >= APP_ADDR && (s_waddr + s_dlen) <= FLASH_END) {
            if (ensure_erased(s_waddr, s_dlen) == 0 &&
                program_block(s_waddr, s_data, s_dlen) == 0) {
              ok = 1;
            }
          }
        }
        ack(ok ? 'w' : '!');
        s_state = S_CMD;
      }
      break;

    case S_G:
      s_hdr[s_hn++] = b;
      if (s_hn == 8u) {
        uint32_t len  = (uint32_t)s_hdr[0] | ((uint32_t)s_hdr[1] << 8)
                      | ((uint32_t)s_hdr[2] << 16) | ((uint32_t)s_hdr[3] << 24);
        uint32_t want = (uint32_t)s_hdr[4] | ((uint32_t)s_hdr[5] << 8)
                      | ((uint32_t)s_hdr[6] << 16) | ((uint32_t)s_hdr[7] << 24);
        s_state = S_CMD;
        if (len <= (FLASH_END - APP_ADDR) &&
            crc32_zlib((const uint8_t *)APP_ADDR, len) == want) {
          ack('g');
          HAL_FLASH_Lock();
          /* Laisser le 'g' partir, puis reset : au reboot -> saut vers l'app. */
          for (volatile uint32_t i = 0; i < 4000000UL; i++) { __NOP(); }
          NVIC_SystemReset();
        } else {
          ack('!');
        }
      }
      break;

    default:
      s_state = S_CMD;
      break;
  }
}

/* ------------------------------------------------------------------------- */
/*  API                                                                       */
/* ------------------------------------------------------------------------- */
void BL_Init(void)
{
  /* Acces au domaine de sauvegarde pour lire le magic DFU (TAMP->BKP0R). */
  __HAL_RCC_PWR_CLK_ENABLE();
  HAL_PWR_EnableBkUpAccess();
#ifdef __HAL_RCC_RTCAPB_CLK_ENABLE
  __HAL_RCC_RTCAPB_CLK_ENABLE();
#endif

  uint32_t magic = TAMP->BKP0R;
  if (magic == BL_DFU_MAGIC) {
    TAMP->BKP0R = 0u;                  /* consomme : le prochain reboot = app */
  } else if (app_valid()) {
    jump_to_app();                     /* ne revient pas                       */
  }

  /* Mode bootloader : preparer le flash et le parser. */
  s_head = 0u; s_tail = 0u;
  s_state = S_CMD; s_hn = 0u; s_dn = 0u; s_dlen = 0u;
  memset(s_erased, 0, sizeof(s_erased));
  HAL_FLASH_Unlock();
}

void BL_RxPush(const uint8_t *buf, uint32_t len)
{
  for (uint32_t i = 0; i < len; i++) {
    uint16_t next = (uint16_t)((s_head + 1u) & RING_MASK);
    if (next == s_tail) { break; }     /* plein : on jette                     */
    s_ring[s_head] = buf[i];
    s_head = next;
  }
}

void BL_Process(void)
{
  while (s_tail != s_head) {
    uint8_t b = s_ring[s_tail];
    s_tail = (uint16_t)((s_tail + 1u) & RING_MASK);
    feed(b);
  }
}
