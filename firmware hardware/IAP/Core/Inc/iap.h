/**
  ******************************************************************************
  * @file    iap.h
  * @brief   Resident USB-CDC bootloader (IAP) - public interface
  ******************************************************************************
  */
#ifndef IAP_H
#define IAP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Big enough for one full WRITE frame: 7 (hdr) + CHUNK + 2 (crc).
   Keep CHUNK on the host <= BL_STASH_SIZE - 16. 2048 fits a 1024-byte chunk. */
#define BL_STASH_SIZE  2048U

/* USB receive stash, filled from CDC_Receive_FS (ISR context) */
extern uint8_t           bl_stash[BL_STASH_SIZE];
extern volatile uint16_t bl_len;

/* Call right after SystemClock_Config(), BEFORE MX_USB_DEVICE_Init().
   Jumps to a valid application (never returns) unless an update is forced
   or no valid app is present, in which case it returns. */
void Bootloader_Boot(void);

/* Call after MX_USB_DEVICE_Init(); runs the update protocol forever. */
void Bootloader_Run(void);

/* zlib-compatible CRC32 (exposed for reuse/testing) */
uint32_t iap_crc32(uint32_t crc, const uint8_t *p, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* IAP_H */
