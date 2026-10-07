#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "Actuator.h"

// ================================================================
//  API de homing MIN non bloquant — remplace MinCalibration.h
//
//  Portage STM32 : seul HardwareAbstraction.h est à adapter
//  (hwReadEndstopStable, hwLedSet, hwLog).
//  Le reste de ce fichier est portable tel quel.
// ================================================================

// --- Vitesse de homing (partagée par tous les axes) ---------------
void     homingSetSpeed(uint32_t interval_us, uint8_t step_size);
uint32_t homingGetIntervalUs();
uint16_t homingGetSps();          // steps/s arrondi
uint8_t  homingGetStepSize();

// --- Contrôle -----------------------------------------------------
// idx_mask : bit N = demander le homing de l'axe N (index tableau)
//   exemple : 0b00110000  →  axes 4 et 5 (M5, M6)
void homingStart(uint8_t idx_mask);
void homingCancel();
void homingTick();                // non bloquant — appeler dans loop()

// --- Abort depuis n'importe quel contexte -------------------------
extern volatile bool g_homingAbort;
inline void homingAbortRequest() { g_homingAbort = true;  }
inline void homingAbortClear()   { g_homingAbort = false; }
inline bool homingAbortCheck()   { return g_homingAbort;  }

// --- Requêtes d'état (idx = index dans ax[], 0-based) -------------
bool homingIsActive (uint8_t idx);   // homing en cours
bool homingIsDone   (uint8_t idx);   // homed == true
bool homingIsError  (uint8_t idx);   // HS_ERROR
