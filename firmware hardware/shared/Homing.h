#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "Actuator.h"

// ================================================================
//  API de homing MIN + calibration complète — remplace MinCalibration.h
//
//  Fichier C pur, partagé AVR / STM32.
//
//  Portage : seules les fonctions hw* ci-dessous sont à fournir par la
//  plateforme (main.c sur STM32, HardwareAbstraction.cpp sur AVR).
//  NOTE AVR : les implés hw* actuelles sont compilées en C++ ; pour lier
//  Homing.c (C), envelopper leurs prototypes en extern "C" dans
//  HardwareAbstraction.h — ou compiler ce module en .cpp côté AVR.
// ================================================================

#ifdef __cplusplus
extern "C" {
#endif

// --- Interface plateforme requise (à fournir ailleurs) -------------
bool hwReadEndstopStable(uint8_t motor, uint16_t blank_us,
                         uint8_t samples, uint8_t need_on);
void hwLedSet(const char* color);
void hwLog(const char* msg);
void refreshConnectionLed(void);
void disableServo(void);
extern bool hostConnected;

// --- Vitesse de homing (partagée par tous les axes) ---------------
void     homingSetSpeed(uint32_t interval_us, uint8_t step_size);
uint32_t homingGetIntervalUs(void);
uint16_t homingGetSps(void);          // steps/s arrondi
uint8_t  homingGetStepSize(void);

// --- Homing MIN (DETECT_MIN) ---------------------------------------
// idx_mask : bit N = demander le homing de l'axe N (index tableau)
//   exemple : 0b00110000  →  axes 4 et 5 (M5, M6)
void homingStart(uint8_t idx_mask);
void homingCancel(void);
void homingTick(void);                // non bloquant — appeler dans loop()

// --- Abort depuis n'importe quel contexte -------------------------
extern volatile bool g_homingAbort;
static inline void homingAbortRequest(void) { g_homingAbort = true;  }
static inline void homingAbortClear(void)   { g_homingAbort = false; }
static inline bool homingAbortCheck(void)   { return g_homingAbort;  }

// --- Requêtes d'état (idx = index dans ax[], 0-based) -------------
bool homingIsActive (uint8_t idx);   // homing en cours
bool homingIsDone   (uint8_t idx);   // homed == true
bool homingIsError  (uint8_t idx);   // HS_ERROR

// ================================================================
//  Calibration complète (au-dessus du homing MIN)
//
//  Équivalents protocole AVR :
//    DO DETECT_MIN  → homingStart(mask)            (zéro posé)
//    DO MOVE_TO_MAX → calibStartDetectMax(idx)     (mesure maxPos)
//    DO FULL_CALIB  → calibStartFull(idx)          (MIN puis MAX)
//    DO CANCEL      → calibCancel() + homingCancel()
//
//  Hypothèse câblage (identique à l'AVR DETECT_MAX_PY) : la MÊME entrée
//  endstop est déclenchée aux deux extrémités de la course (switches MIN
//  et MAX en série sur l'entrée M5/M6 ENDSTOP).
//
//  En fin de DETECT_MAX réussi : axCfg[idx].maxPos = position après
//  dégagement, ax[idx].calibrated = true, ax[idx].homed = true.
//  La PERSISTANCE (NVM/EEPROM) est laissée à l'appelant, qui détecte la
//  fin via calibIsBusy()/calibResult().
// ================================================================

typedef enum {
    CAL_IDLE = 0,
    CAL_MIN,           // homing MIN en cours (délégué à la FSM homing)
    CAL_MAX_SEEK,      // avance vers MAX, cherche l'endstop
    CAL_MAX_RELEASE,   // endstop MAX touché → dégagement + mesure maxPos
    CAL_DONE,          // séquence terminée avec succès
    CAL_ERROR          // échec (timeout, abort, homing failed)
} CalibState;

void calibStartDetectMax(uint8_t idx);  // exige ax[idx].homed
void calibStartFull(uint8_t idx);       // MIN puis MAX
void calibCancel(void);
void calibTick(void);                   // non bloquant — appeler dans loop()

bool       calibIsBusy(uint8_t idx);    // séquence en cours
CalibState calibGetState(uint8_t idx);  // CAL_DONE / CAL_ERROR consultables
void       calibAckResult(uint8_t idx); // repasse DONE/ERROR → IDLE

#ifdef __cplusplus
}  // extern "C"
#endif
