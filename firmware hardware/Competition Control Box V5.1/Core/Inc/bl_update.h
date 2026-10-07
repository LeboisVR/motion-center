/**
 ******************************************************************************
 * @file    bl_update.h
 * @brief   Mise a jour du bootloader resident depuis l'application (Bank 2).
 *
 *  Protocole identique au bootloader CDC (P/E/W/G), mais l'application est
 *  l'hote : elle tourne depuis Bank 2 et programme Bank 1 via RWW.
 *
 *  Usage depuis mc_protocol.c :
 *    MC_HandleLine("!BL_UPDATE") -> BL_Update_Enter() -> envoie "BL_UPDATE_READY\n"
 *
 *  Usage depuis usbd_cdc_if.c (CDC_Receive_FS) :
 *    if (BL_Update_IsActive()) BL_Update_RxPush(Buf, *Len);
 *    else                      SimHub_RxPush(Buf, *Len);
 *
 *  Usage depuis la boucle principale (main.c) :
 *    BL_Update_Process();
 ******************************************************************************
 */
#ifndef BL_UPDATE_H
#define BL_UPDATE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief  Entre en mode "mise a jour bootloader".
 *         Arrete le servo, deverrouille le flash, remet le parser a zero
 *         et envoie "BL_UPDATE_READY\n" sur CDC.
 */
void BL_Update_Enter(void);

/** @return true si le mode mise a jour bootloader est actif. */
bool BL_Update_IsActive(void);

/**
 * @brief  Copie les octets recus en USB CDC dans le ring buffer interne.
 *         A appeler DEPUIS L'INTERRUPTION CDC (CDC_Receive_FS).
 */
void BL_Update_RxPush(const uint8_t *buf, uint32_t len);

/**
 * @brief  Traite les octets en attente (P/E/W/G state machine).
 *         A appeler depuis la boucle principale.
 */
void BL_Update_Process(void);

#ifdef __cplusplus
}
#endif
#endif /* BL_UPDATE_H */
