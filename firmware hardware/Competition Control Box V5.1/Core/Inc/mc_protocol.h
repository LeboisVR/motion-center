/**
 ******************************************************************************
 * @file    mc_protocol.h
 * @brief   Compatibilite Motion Center : commandes texte ASCII + DFU.
 *
 *  Protocole texte (lignes terminees '\n', extraites par simhub_parser) :
 *    HELLO                 -> "OK_MC_CONNECTED"   (detection app)
 *    GET                   -> ligne "STATUS FW=.. BOX=.. SERVO=.. .."
 *    ENABLE                -> active les drivers (servo ON)
 *    DISABLE / MC_DISABLE  -> coupe les drivers (servo OFF) + arret moteurs
 *    DISCONNECT            -> deconnexion logique (servo OFF + arret)
 *    DO SERVO_ON|SERVO_OFF -> idem ENABLE/DISABLE
 *    DO FACTORY_RESET      -> "OK" (reset reglages : stub NVM a venir)
 *    SET <KEY> <VAL> [m]   -> "OK"  (HOMING_S, ESTOP, CONNECTED, MAX, MARGIN)
 *    STEP <m> <delta>      -> jog manuel relatif
 *    !DFU                  -> "DFU" puis reboot vers le bootloader resident
 *
 *  NOTE DFU : l'entree bootloader pose un magic puis NVIC_SystemReset().
 *  Elle suppose la presence d'un bootloader USB-CDC resident a 0x08000000
 *  (app liee a 0x08005000) qui relit ce magic. Ce bootloader G4 n'existe
 *  pas encore (cf. mc_protocol.c).
 ******************************************************************************
 */
#ifndef MC_PROTOCOL_H
#define MC_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialise l'etat (servo OFF). A appeler une fois au boot. */
void MC_Init(void);

/** Service periodic tasks (currently used for deferred ESTOP handling). */
void MC_Process(void);

/**
 * @brief Traite une ligne texte recue.
 * @return true si la commande a ete reconnue et traitee, false sinon.
 */
bool MC_HandleLine(const char *line);

/** @return true si le servo (drivers) est actif. */
bool MC_ServoEnabled(void);

/** Active uniquement les moteurs marques CONNECTED et alimente le relais. */
void MC_EnableConnectedMotors(void);

/** Force servo OFF (drivers disabled + motion stopped). */
void MC_DisableServo(void);

/** Endpark : rejoint doucement le %% de course configure (M5..M7) au
    SH_DISABLE, avant la coupure servo. Bloquant ; no-op si servo OFF ou
    endpark desactive (255). */
void MC_RunEndPark(void);

/** Log per-axis enable status (connected, ready, enabled). */
void MC_LogEnabledStatus(void);

/** Log ESTOP mode and current ESTOP pin state. */
void MC_LogEstopStatus(void);

/**
 * @return true when ESTOP mode is enabled and ESTOP input is active.
 *         Motion command streams should be ignored while this is true.
 */
bool MC_IsEstopMotionBlocked(void);

/** @return true when a safety lock requires a full reboot before restart. */
bool MC_IsRestartRequired(void);

/** Run startup homing/calibration for all connected motors. */
bool MC_RunStartupCalibration(void);
/* Raison du dernier echec de calibration (chaine courte, "" si aucun). */
const char *MC_GetCalFailReason(void);

/** Run homing/calibration on one motor index (1..MOTION_NUM_AXES). */
/** Run MIN homing calibration for one motor (1-based). Auto-connects the
    motor (RAM) like the AVR autoConnectIfNeeded rule. */
bool MC_RunMotorCalibration(uint8_t motor1Based);

/** Full calibration for optional actuators M5..M7 : MIN homing then MAX
    stroke measurement (real max can be < 32767). M1..M4 fall back to MIN
    homing only. */
bool MC_RunMotorFullCalibration(uint8_t motor1Based);

/** Mise au centre d'un verin (1-based) : re-reference MIN si la position
    n'est pas connue dans la session, puis rejoint le milieu de course. */
bool MC_GoToCenter(uint8_t motor1Based);

/** Detection/mesure de la butee MAX (1-based). Re-reference MIN si besoin,
    puis mesure la course MAX et remonte STROKE Mx. */
bool MC_MoveToMax(uint8_t motor1Based);

/** Test de course complete (1-based) : min -> max -> centre. */
bool MC_TestStroke(uint8_t motor1Based);

/** Mappe une consigne SimHub 15 bits (0..32767) vers la cible en pas de l'axe
    `idx` (0-based) en appliquant le max/marge calibres (PosMap). */
int32_t MC_MapAxisTarget(uint8_t idx, uint16_t u);

/**
 * @return Bitmask of motors whose READY pin is active (bit 0 = M1 ... bit N-1).
 *         0 means no motor is ready.
 */
uint8_t MC_GetReadyMask(void);

#ifdef __cplusplus
}
#endif

#endif /* MC_PROTOCOL_H */
