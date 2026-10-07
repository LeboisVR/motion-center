// Code version 1.0

#ifndef MINCALIBRATION_H
#define MINCALIBRATION_H

#include <Arduino.h>
#pragma once
#include <Arduino.h>

extern volatile bool g_abortRequested;
inline void calibAbortRequest() { g_abortRequested = true; }
inline void calibAbortClear()   { g_abortRequested = false; }
inline bool calibAbortCheck()   { return g_abortRequested; }
bool isHomingIdx(uint8_t i);
// Lance le homing MIN non bloquant (lent) pour M5/M6.
// doM5 / doM6 : true pour (re)lancer l'axe correspondant.
void startMinCalibration(bool doM5, bool doM6);

// Annule toute calibration en cours pour M5/M6.
void cancelMinCalibration();

// À appeler très souvent (loop). Planifie des pas lents vers le MIN
// pour les axes en calibration sans bloquer la comm ni M1–M4.
void tickMinCalibration();

// (Optionnel) Réglage runtime de la "lenteur":
// - interval_us : microsecondes entre deux pas autorisés (par axe) en homing
// - step_size   : nombre de pas ajoutés/soustraits par "tick" (>=1)
void setMinCalibSpeed(uint32_t interval_us, uint8_t step_size);

// (Optionnel) Helpers d’état
bool isMinCalibrating(uint8_t motor);  // motor = 5 ou 6
bool isMinCalibrated(uint8_t motor);   // motor = 5 ou 6

void setMinCalibSpeed(uint32_t interval_us, uint8_t step_size);
uint32_t getMinCalibStepIntervalUs();
uint16_t getMinCalibStepsPerSecond();
uint8_t  getMinCalibStepSize();

#endif // MINCALIBRATION_H