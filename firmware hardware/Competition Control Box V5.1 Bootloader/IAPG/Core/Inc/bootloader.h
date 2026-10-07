/**
 ******************************************************************************
 * @file    bootloader.h
 * @brief   Bootloader USB-CDC resident pour STM32G474VET6.
 *
 *  Permet la mise a jour du firmware applicatif depuis Motion Center, sur le
 *  MEME port COM (aucun changement de driver Windows). Protocole identique au
 *  bootloader F103 -> le flasheur _flash_worker_stm32 de Motion Center
 *  fonctionne SANS modification (STM32_APP_ADDR = 0x08005000).
 *
 *  ----------------------------------------------------------------------------
 *  CARTE FLASH (512 Ko, double banque)
 *    BANQUE 1  0x08000000 .. 0x08040000  (256 Ko) : ce bootloader
 *    BANQUE 2  0x08040000 .. 0x08080000  (256 Ko) : application
 *  L'app est en banque 2 : le bootloader (banque 1) programme la banque 2 sans
 *  se bloquer (read-while-write inter-banque du G4). STM32_APP_ADDR cote
 *  Motion Center = 0x08040000 pour cette cible.
 *  ----------------------------------------------------------------------------
 *  PROTOCOLE (little-endian), reponses 1 octet sauf 'P' :
 *    'P'                                -> 'p' + octet version      (PING)
 *    'E'                                -> 'e' | '!'   (arme l'effacement)
 *    'W' addr(4) len(2) data crc16(2)   -> 'w' | '!'   (CRC16-CCITT 0xFFFF)
 *    'G' len(4) crc32(4)                -> 'g' | '!'   (CRC32 zlib image)
 *  Sur 'g' : reset -> au reboot le bootloader saute vers l'app validee.
 *  Tout octet inattendu est ignore (resync : absorbe "!DFU\n").
 *
 *  ANTI-BRICK : effacement PARESSEUX par page (a la 1ere ecriture sur la page).
 *    - interrompu avant la 1ere ecriture  -> ancienne app intacte
 *    - interrompu apres                    -> page vecteur invalide -> bootloader
 *  ----------------------------------------------------------------------------
 *  PROJET CubeMX A GENERER (separe de l'application) :
 *    - STM32G474VETx, USB_DEVICE classe CDC (VCP), horloge USB OK
 *      (HSI48 + CRS, ou PLLQ 48 MHz).
 *    - AUCUN peripherique applicatif (pas de timers moteur, etc.).
 *    - Linker  : FLASH ORIGIN = 0x08000000, LENGTH = 256K (banque 1).
 *    - VTOR    : laisser VECT_TAB_OFFSET = 0 (bootloader a la base du flash).
 *    - Cablage : appeler BL_Init() dans USER CODE BEGIN 1 (AVANT HAL_Init),
 *                BL_Process() dans la boucle while(1),
 *                et BL_RxPush(Buf,*Len) dans CDC_Receive_FS().
 ******************************************************************************
 */
#ifndef BOOTLOADER_H
#define BOOTLOADER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A appeler TOUT AU DEBUT de main() (USER CODE BEGIN 1, avant HAL_Init).
 *        Si aucune demande DFU et qu'une application valide est presente, saute
 *        immediatement vers l'application (ne revient pas). Sinon revient pour
 *        laisser main() initialiser l'USB et servir le bootloader.
 */
void BL_Init(void);

/** @brief Boucle de service du protocole (a appeler dans while(1)). */
void BL_Process(void);

/** @brief A appeler depuis CDC_Receive_FS() avec les octets recus. */
void BL_RxPush(const uint8_t *buf, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* BOOTLOADER_H */
