/**
 ******************************************************************************
 * @file    bl_update.c
 * @brief   Mise a jour du bootloader resident depuis l'application (Bank 2).
 *
 *  L'application est liee en Bank 2 (0x08040000). Elle peut donc programmer
 *  Bank 1 (0x08000000) via le mecanisme Read-While-Write (RWW) du STM32G4
 *  sans se "tirer une balle dans le pied". Le protocole P/E/W/G est identique
 *  a celui du bootloader resident, pour reutiliser le meme worker Motion Center.
 *
 *  AVERTISSEMENT SECURITE :
 *    Une interruption pendant l'ecriture de Bank 1 rend la carte
 *    inaccessible sans ST-Link (BOOT0). C'est le risque inherent a toute
 *    mise a jour de bootloader. L'ecriture lazy (page par page) minimise
 *    la fenetre : seules les pages deja ecrites sont effacees.
 ******************************************************************************
 */
#include "bl_update.h"
#include "cdc_tx.h"
#include "mc_protocol.h"
#include "main.h"
#include "usb_device.h"
#include "usbd_cdc.h"
#include "usbd_cdc_if.h"
#include <string.h>

extern USBD_HandleTypeDef hUsbDeviceFS;

/* ------------------------------------------------------------------------- */
/*  Configuration                                                             */
/* ------------------------------------------------------------------------- */
#define BLU_VERSION       1u              /* rapporte au PING 'P'             */

#define BL_BASE_ADDR      0x08000000UL   /* debut Bank 1 = debut bootloader   */
#define BL_END_ADDR       0x08040000UL   /* fin Bank 1 (256 Ko)               */
#define PAGE_SIZE         0x800UL        /* 2 Ko par page (G474 dual-bank)    */
#define NUM_BL_PAGES      ((BL_END_ADDR - BL_BASE_ADDR) / PAGE_SIZE)  /* 128 */

#define W_DATA_MAX        1024u

/* ------------------------------------------------------------------------- */
/*  Ring buffer USB (rempli en IT, vide en boucle principale)                */
/* ------------------------------------------------------------------------- */
#define RING_SIZE   2048u
#define RING_MASK   (RING_SIZE - 1u)
static volatile uint8_t  s_ring[RING_SIZE];
static volatile uint16_t s_head;
static volatile uint16_t s_tail;

/* Bitmap des pages deja effacees durant la session (1 bit / page). */
static uint8_t s_erased[(NUM_BL_PAGES + 7u) / 8u];

/* ------------------------------------------------------------------------- */
/*  Etat du module                                                            */
/* ------------------------------------------------------------------------- */
static volatile bool s_active;   /* true = mode mise a jour BL actif        */

/* ------------------------------------------------------------------------- */
/*  Parser state machine                                                      */
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
/*  Helpers TX                                                                */
/* ------------------------------------------------------------------------- */
static void blu_send(const uint8_t *p, uint16_t n)
{
  static uint8_t txbuf[4];
  if (n > sizeof(txbuf)) { n = sizeof(txbuf); }
  USBD_CDC_HandleTypeDef *hcdc =
      (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;
  if (hcdc == NULL) { return; }
  for (uint32_t k = 0u; (k < 200000u) && (hcdc->TxState != 0u); k++) { __NOP(); }
  if (hcdc->TxState != 0u) { return; }
  memcpy(txbuf, p, n);
  for (uint16_t k = 0u; k < 64u; k++) {
    if (CDC_Transmit_FS(txbuf, n) == USBD_OK) { return; }
    for (uint32_t d = 0u; d < 2000u; d++) { __NOP(); }
  }
}

static void blu_ack(uint8_t c) { blu_send(&c, 1u); }

/* ------------------------------------------------------------------------- */
/*  CRC (identiques au bootloader et a Motion Center)                        */
/* ------------------------------------------------------------------------- */
static uint16_t crc16_ccitt(const uint8_t *d, uint16_t len)  /* == crc_hqx */
{
  uint16_t crc = 0xFFFFu;
  for (uint16_t i = 0u; i < len; i++) {
    crc ^= (uint16_t)((uint16_t)d[i] << 8);
    for (uint8_t b = 0u; b < 8u; b++) {
      crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                            : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

static uint32_t crc32_zlib(const uint8_t *d, uint32_t len)
{
  uint32_t crc = 0xFFFFFFFFUL;
  for (uint32_t i = 0u; i < len; i++) {
    crc ^= d[i];
    for (uint8_t b = 0u; b < 8u; b++) {
      crc = (crc >> 1) ^ (0xEDB88420UL & (uint32_t)(-(int32_t)(crc & 1u)));
    }
  }
  return ~crc;
}

/* ------------------------------------------------------------------------- */
/*  Flash Bank 1 (RWW depuis Bank 2)                                         */
/* ------------------------------------------------------------------------- */
static int erase_bl_page(uint32_t addr)
{
  FLASH_EraseInitTypeDef e;
  e.TypeErase = FLASH_TYPEERASE_PAGES;
  e.Banks     = FLASH_BANK_1;
  e.Page      = (uint32_t)((addr - BL_BASE_ADDR) / PAGE_SIZE);
  e.NbPages   = 1u;
  uint32_t err = 0u;
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
  return (HAL_FLASHEx_Erase(&e, &err) == HAL_OK) ? 0 : -1;
}

/* Efface (une seule fois par session) toutes les pages couvrant [addr, addr+len). */
static int ensure_erased(uint32_t addr, uint16_t len)
{
  uint32_t a   = addr;
  uint32_t end = (uint32_t)(addr + len);
  while (a < end) {
    uint32_t idx = (a - BL_BASE_ADDR) / PAGE_SIZE;
    if (idx < NUM_BL_PAGES) {
      uint8_t mask = (uint8_t)(1u << (idx & 7u));
      if ((s_erased[idx >> 3u] & mask) == 0u) {
        if (erase_bl_page(a & ~(PAGE_SIZE - 1u)) != 0) { return -1; }
        s_erased[idx >> 3u] |= mask;
      }
    }
    a = (a & ~(PAGE_SIZE - 1u)) + PAGE_SIZE;
  }
  return 0;
}

/* Programme par double-mot (64 bits, seule granularite du G474).
   Les octets manquants du dernier mot sont remplis a 0xFF. */
static int program_block(uint32_t addr, const uint8_t *data, uint16_t len)
{
  for (uint16_t i = 0u; i < len; i += 8u) {
    uint64_t dw = 0xFFFFFFFFFFFFFFFFULL;
    uint8_t  n  = ((uint16_t)(len - i) >= 8u) ? 8u : (uint8_t)(len - i);
    for (uint8_t k = 0u; k < n; k++) {
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
/*  Parser : traite un octet                                                  */
/* ------------------------------------------------------------------------- */
static void feed(uint8_t b)
{
  switch (s_state) {

    case S_CMD:
      if (b == 'P') {
        /* PING : repond 'p' + version (2 octets binaires). */
        uint8_t resp[2] = { 'p', (uint8_t)BLU_VERSION };
        blu_send(resp, 2u);
      } else if (b == 'E') {
        /* ERASE : arme le bitmap (erase paresseux sur premier write) */
        memset(s_erased, 0, sizeof(s_erased));
        blu_ack('e');
      } else if (b == 'W') {
        s_hn = 0u; s_state = S_W_HDR;
      } else if (b == 'G') {
        s_hn = 0u; s_state = S_G;
      }
      /* Tous les autres octets sont ignores (resync). */
      break;

    case S_W_HDR:
      s_hdr[s_hn++] = b;
      if (s_hn == 6u) {
        s_waddr = (uint32_t)s_hdr[0]
                | ((uint32_t)s_hdr[1] << 8)
                | ((uint32_t)s_hdr[2] << 16)
                | ((uint32_t)s_hdr[3] << 24);
        s_dlen  = (uint16_t)((uint16_t)s_hdr[4]
                | ((uint16_t)s_hdr[5] << 8));
        if (s_dlen > W_DATA_MAX) { blu_ack('!'); s_state = S_CMD; break; }
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
        uint16_t want = (uint16_t)((uint16_t)s_hdr[0]
                      | ((uint16_t)s_hdr[1] << 8));
        int ok = 0;
        if (crc16_ccitt(s_data, s_dlen) == want) {
          /* Adresse cible dans Bank 1 uniquement. */
          if (s_waddr >= BL_BASE_ADDR
              && (s_waddr + (uint32_t)s_dlen) <= BL_END_ADDR) {
            if (ensure_erased(s_waddr, s_dlen) == 0
                && program_block(s_waddr, s_data, s_dlen) == 0) {
              ok = 1;
            }
          }
        }
        blu_ack(ok ? 'w' : '!');
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
        if (len <= (BL_END_ADDR - BL_BASE_ADDR)
            && crc32_zlib((const uint8_t *)BL_BASE_ADDR, len) == want) {
          blu_ack('g');
          HAL_FLASH_Lock();
          /* Laisser le 'g' partir avant le reset. */
          for (volatile uint32_t i = 0u; i < 4000000UL; i++) { __NOP(); }
          NVIC_SystemReset();
        } else {
          blu_ack('!');
        }
      }
      break;

    default:
      s_state = S_CMD;
      break;
  }
}

/* ------------------------------------------------------------------------- */
/*  API publique                                                              */
/* ------------------------------------------------------------------------- */

void BL_Update_Enter(void)
{
  /* Arreter le servo et la motion avant de verrouiller le CPU sur le flash. */
  MC_DisableServo();

  /* Initialiser le parser et le ring buffer. */
  s_head  = 0u; s_tail = 0u;
  s_state = S_CMD; s_hn = 0u; s_dn = 0u; s_dlen = 0u;
  memset(s_erased, 0, sizeof(s_erased));

  HAL_FLASH_Unlock();

  /* Signaler a usbd_cdc_if.c que les octets CDC vont vers ce module. */
  s_active = true;

  /* Annoncer la disponibilite. */
  CDC_SendStr("BL_UPDATE_READY\n");
}

bool BL_Update_IsActive(void)
{
  return s_active;
}

void BL_Update_RxPush(const uint8_t *buf, uint32_t len)
{
  for (uint32_t i = 0u; i < len; i++) {
    uint16_t next = (uint16_t)((s_head + 1u) & RING_MASK);
    if (next == s_tail) { break; }    /* ring plein : jette (ne doit pas arriver) */
    s_ring[s_head] = buf[i];
    s_head = next;
  }
}

void BL_Update_Process(void)
{
  if (!s_active) { return; }
  while (s_tail != s_head) {
    uint8_t b = s_ring[s_tail];
    s_tail = (uint16_t)((s_tail + 1u) & RING_MASK);
    feed(b);
  }
}
