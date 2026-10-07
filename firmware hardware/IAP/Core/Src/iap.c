/**
  ******************************************************************************
  * @file    iap.c
  * @brief   Resident USB-CDC bootloader (IAP) for STM32F103C8 / Blue Pill
  *
  *  Memory map (64 KB flash, 1 KB pages):
  *    Bootloader : 0x08000000 - 0x08004FFF  (20 KB,  pages 0..19)
  *    Application: 0x08005000 - 0x0800FBFF  (43 KB,  pages 20..62)
  *    Metadata   : 0x0800FC00 - 0x0800FFFF  ( 1 KB,  page 63)
  *
  *  This module is meant to be dropped into a CubeMX-generated project for
  *  the F103C8 with USB Device (CDC) enabled and the SAME clock config as the
  *  application (HSE 8 MHz, PLL x9 = 72 MHz, USB = PLL/1.5 = 48 MHz).
  *
  *  In the bootloader project:
  *    - call Bootloader_Boot() at the very top of main(), BEFORE the USB init,
  *      right after HAL_Init() + SystemClock_Config(). It either jumps to a
  *      valid app or returns (meaning: stay here and run update mode).
  *    - after MX_USB_DEVICE_Init(), call Bootloader_Run() in the main loop.
  *    - patch CDC_Receive_FS() to append into bl_stash (see bottom of file).
  ******************************************************************************
  */

#include "iap.h"
#include "stm32f1xx_hal.h"
#include "usbd_cdc_if.h"
#include <string.h>

/* ---- Layout -------------------------------------------------------------- */
#define APP_ADDR        0x08005000UL
#define MDATA_ADDR      0x0800FC00UL          /* last page: metadata          */
#define APP_END         MDATA_ADDR            /* exclusive                    */
#define PAGE_SIZE       1024UL
#define APP_FIRST_PAGE  20U                   /* 0x5000 / 0x400               */
#define APP_LAST_PAGE   63U                   /* includes metadata page       */

#define META_MAGIC      0xB007AC01UL          /* "boot app ok"                */
#define BKP_FORCE_MAGIC 0x4F50U               /* 'OP' written by app's !DFU   */
#define FW_VERSION      0x01U

/* All reads of flash content go through FLASH_PTR so the module can be unit
   tested on a host with a RAM-backed mock (define IAP_HOST_TEST). On target
   it compiles to a plain cast — zero cost. */
#ifdef IAP_HOST_TEST
extern uint8_t *iap_host_flash_ptr(uint32_t addr);
#define FLASH_PTR(a)  iap_host_flash_ptr(a)
#else
#define FLASH_PTR(a)  ((uint8_t *)(uintptr_t)(a))
#endif

/* Metadata layout written into the last flash page */
typedef struct {
  uint32_t magic;
  uint32_t length;
  uint32_t crc32;
} meta_t;

/* ---- USB receive stash (filled from CDC_Receive_FS, ISR context) --------- */
uint8_t           bl_stash[BL_STASH_SIZE];
volatile uint16_t bl_len;

/* ========================================================================== */
/*  CRC                                                                        */
/* ========================================================================== */

/* zlib-compatible CRC32 (reflected, poly 0xEDB88320). crc32(0, ...) */
uint32_t iap_crc32(uint32_t crc, const uint8_t *p, uint32_t len)
{
  crc = ~crc;
  while (len--) {
    crc ^= *p++;
    for (int k = 0; k < 8; k++)
      crc = (crc >> 1) ^ (0xEDB88320UL & (~(crc & 1UL) + 1UL));
  }
  return ~crc;
}

/* CRC16-CCITT (poly 0x1021, init 0xFFFF) for per-block transport integrity */
static uint16_t crc16_ccitt(const uint8_t *p, uint16_t len)
{
  uint16_t crc = 0xFFFF;
  while (len--) {
    crc ^= (uint16_t)(*p++) << 8;
    for (int i = 0; i < 8; i++)
      crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

/* ========================================================================== */
/*  Backup-register force-update flag                                          */
/* ========================================================================== */

static void bkp_clock_on(void)
{
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_RCC_BKP_CLK_ENABLE();
  HAL_PWR_EnableBkUpAccess();
}

static uint8_t force_update_requested(void)
{
  bkp_clock_on();
  if (BKP->DR1 == BKP_FORCE_MAGIC) {
    BKP->DR1 = 0;           /* one-shot: clear it so we don't loop */
    return 1U;
  }
  return 0U;
}

/* ========================================================================== */
/*  Flash                                                                      */
/* ========================================================================== */

static int flash_erase_app(void)
{
  FLASH_EraseInitTypeDef ei = {0};
  uint32_t pageErr = 0;
  ei.TypeErase   = FLASH_TYPEERASE_PAGES;
  ei.Banks       = FLASH_BANK_1;
  ei.PageAddress = APP_ADDR;
  ei.NbPages     = (APP_LAST_PAGE - APP_FIRST_PAGE) + 1U;   /* incl. metadata */

  HAL_FLASH_Unlock();
  HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&ei, &pageErr);
  HAL_FLASH_Lock();
  return (st == HAL_OK) ? 0 : -1;
}

/* Program a buffer (half-word granularity). addr & len must be even. */
static int flash_write(uint32_t addr, const uint8_t *data, uint16_t len)
{
  if (addr < APP_ADDR || (addr + len) > MDATA_ADDR + PAGE_SIZE) return -1; /* out of bounds */
  if ((addr & 1U) || (len & 1U)) return -1;

  HAL_FLASH_Unlock();
  for (uint16_t i = 0; i < len; i += 2) {
    uint16_t hw = (uint16_t)(data[i] | (data[i + 1] << 8));
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_HALFWORD, addr + i, hw) != HAL_OK) {
      HAL_FLASH_Lock();
      return -1;
    }
    /* read-back verify */
    uint16_t rb = (uint16_t)(FLASH_PTR(addr + i)[0] | (FLASH_PTR(addr + i)[1] << 8));
    if (rb != hw) {
      HAL_FLASH_Lock();
      return -1;
    }
  }
  HAL_FLASH_Lock();
  return 0;
}

static int meta_write(uint32_t length, uint32_t crc)
{
  meta_t m = { META_MAGIC, length, crc };
  return flash_write(MDATA_ADDR, (const uint8_t *)&m, sizeof(m));
}

/* ========================================================================== */
/*  App validity + jump                                                        */
/* ========================================================================== */

static uint8_t app_is_valid(void)
{
  meta_t m;
  memcpy(&m, FLASH_PTR(MDATA_ADDR), sizeof(m));
  if (m.magic != META_MAGIC) return 0U;
  if (m.length == 0U || m.length > (APP_END - APP_ADDR)) return 0U;

  uint32_t c = iap_crc32(0, FLASH_PTR(APP_ADDR), m.length);
  return (c == m.crc32) ? 1U : 0U;
}

typedef void (*pfn_t)(void);

static void jump_to_app(void)
{
  uint32_t sp, pc;
  memcpy(&sp, FLASH_PTR(APP_ADDR),      4U);
  memcpy(&pc, FLASH_PTR(APP_ADDR + 4U), 4U);

  /* sanity: stack pointer must land in SRAM (0x2000xxxx) */
  if ((sp & 0xFFFE0000U) != 0x20000000U) return;

  __disable_irq();
  HAL_RCC_DeInit();
  HAL_DeInit();
  SysTick->CTRL = 0; SysTick->LOAD = 0; SysTick->VAL = 0;
  for (uint8_t i = 0; i < 8U; i++) { NVIC->ICER[i] = 0xFFFFFFFFU; NVIC->ICPR[i] = 0xFFFFFFFFU; }

  SCB->VTOR = APP_ADDR;
  __set_MSP(sp);
  __enable_irq();

  pfn_t app = (pfn_t)pc;
  app();
  while (1) {}
}

/* ========================================================================== */
/*  Public boot entry: jump if valid & not forced, else return to run update   */
/* ========================================================================== */

void Bootloader_Boot(void)
{
  uint8_t forced = force_update_requested();
  if (!forced && app_is_valid()) {
    jump_to_app();   /* does not return */
  }
  /* else: fall through -> caller inits USB and calls Bootloader_Run() */
}

/* ========================================================================== */
/*  Protocol over CDC                                                          */
/* ========================================================================== */

static void send(const uint8_t *p, uint16_t n)
{
  for (uint16_t k = 0; k < 200U; k++) {
    if (CDC_Transmit_FS((uint8_t *)p, n) == USBD_OK) return;
    HAL_Delay(1);
  }
}
static void send_byte(uint8_t b) { send(&b, 1); }

static void stash_drop(uint16_t n)
{
  __disable_irq();
  if (n >= bl_len) { bl_len = 0; }
  else { memmove(bl_stash, bl_stash + n, bl_len - n); bl_len -= n; }
  __enable_irq();
}

static inline uint32_t rd32(const uint8_t *p)
{ return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline uint16_t rd16(const uint8_t *p)
{ return (uint16_t)(p[0] | (p[1] << 8)); }

/* Try to parse and execute one frame at the head of the stash.
   Returns 1 if a frame was consumed, 0 if waiting for more bytes. */
static uint8_t process_one(void)
{
  if (bl_len == 0U) return 0U;

  switch (bl_stash[0]) {

    case 'P':                                   /* PING */
      stash_drop(1U);
      { uint8_t r[2] = { 'p', FW_VERSION }; send(r, 2); }
      return 1U;

    case 'E':                                   /* ERASE app region */
      stash_drop(1U);
      send_byte(flash_erase_app() == 0 ? 'e' : '!');
      return 1U;

    case 'W': {                                 /* WRITE addr(4) len(2) data crc16(2) */
      if (bl_len < 7U) return 0U;               /* need header first */
      uint32_t addr = rd32(&bl_stash[1]);
      uint16_t len  = rd16(&bl_stash[5]);
      if (len == 0U || len > (BL_STASH_SIZE - 16U)) { stash_drop(1U); send_byte('!'); return 1U; }
      uint16_t total = (uint16_t)(7U + len + 2U);
      if (bl_len < total) return 0U;            /* wait for full payload */

      const uint8_t *data = &bl_stash[7];
      uint16_t rx_crc = rd16(&bl_stash[7U + len]);
      if (crc16_ccitt(data, len) != rx_crc) { stash_drop(total); send_byte('!'); return 1U; }

      int ok = flash_write(addr, data, len);
      stash_drop(total);
      send_byte(ok == 0 ? 'w' : '!');
      return 1U;
    }

    case 'G': {                                 /* GO len(4) crc32(4) */
      if (bl_len < 9U) return 0U;
      uint32_t len  = rd32(&bl_stash[1]);
      uint32_t crc  = rd32(&bl_stash[5]);
      stash_drop(9U);

      if (len == 0U || len > (APP_END - APP_ADDR)) { send_byte('!'); return 1U; }
      uint32_t actual = iap_crc32(0, FLASH_PTR(APP_ADDR), len);
      if (actual != crc) { send_byte('!'); return 1U; }
      if (meta_write(len, crc) != 0) { send_byte('!'); return 1U; }

      send_byte('g');
      HAL_Delay(50);                            /* let ACK leave */
      NVIC_SystemReset();                       /* reboot -> validate -> jump */
      return 1U;                                /* not reached */
    }

    default:                                    /* resync: drop one byte */
      stash_drop(1U);
      return 1U;
  }
}

void Bootloader_Run(void)
{
  for (;;) {
    while (process_one()) { }
    /* idle */
  }
}

/* ==========================================================================
 *  PATCH for usbd_cdc_if.c in the BOOTLOADER project
 *  ---------------------------------------------------------------------------
 *  Add near the top (USER CODE / PRIVATE_VARIABLES):
 *
 *      #include <string.h>
 *      #include "iap.h"
 *
 *  Replace the body of CDC_Receive_FS() with:
 *
 *      static int8_t CDC_Receive_FS(uint8_t* Buf, uint32_t *Len)
 *      {
 *        uint32_t n = *Len;
 *        if (n > 0U && (uint32_t)bl_len + n <= BL_STASH_SIZE) {
 *          memcpy(&bl_stash[bl_len], Buf, n);
 *          bl_len = (uint16_t)(bl_len + n);
 *        }
 *        USBD_CDC_SetRxBuffer(&hUsbDeviceFS, &Buf[0]);
 *        USBD_CDC_ReceivePacket(&hUsbDeviceFS);
 *        return (USBD_OK);
 *      }
 * ========================================================================== */
