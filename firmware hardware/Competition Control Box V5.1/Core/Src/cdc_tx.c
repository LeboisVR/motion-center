/**
 ******************************************************************************
 * @file    cdc_tx.c
 * @brief   Envoi de chaines ASCII sur l'USB CDC — implementation.
 ******************************************************************************
 */
#include "cdc_tx.h"
#include <string.h>
#include "main.h"

#include "usb_device.h"        /* hUsbDeviceFS                               */
#include "usbd_cdc.h"          /* USBD_CDC_HandleTypeDef                     */
#include "usbd_cdc_if.h"       /* CDC_Transmit_FS                            */

extern USBD_HandleTypeDef hUsbDeviceFS;

void CDC_SendStr(const char *s)
{
  /* 320 >= send_status() buffer (288) : une ligne STATUS 7 moteurs faisait
     193 octets et perdait son '\n' avec l'ancien buffer de 192. */
  static uint8_t txbuf[320];
  uint16_t n = (uint16_t)strlen(s);
  if (n == 0u) { return; }
  if (n > sizeof(txbuf)) { n = (uint16_t)sizeof(txbuf); }

  USBD_CDC_HandleTypeDef *hcdc =
      (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;
  if (hcdc == NULL) { return; }             /* USB pas enumere               */

  /* Attendre la fin du transfert precedent (~5 ms max). */
  for (uint32_t k = 0; (k < 50000u) && (hcdc->TxState != 0u); k++) { __NOP(); }
  if (hcdc->TxState != 0u) { return; }

  memcpy(txbuf, s, n);
  for (uint16_t k = 0; k < 64u; k++) {
    if (CDC_Transmit_FS(txbuf, n) == USBD_OK) { return; }
    for (uint32_t d = 0; d < 2000u; d++) { __NOP(); }
  }
}
