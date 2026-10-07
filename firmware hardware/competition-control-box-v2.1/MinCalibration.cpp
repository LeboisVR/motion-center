#include "MinCalibration.h"
#include "HardwareAbstraction.h"
#include "MotorControl.h"
#include "Globals.h"
#include <Arduino.h>
#include "CalibrationUtils.h"

volatile bool g_abortRequested = false;

// ================== Nouveaux états clairs ==================
extern bool mConnected[MAX_ACTUATORS];   // défini ailleurs (détection, config…)
extern bool calibrated[MAX_ACTUATORS];   // min/max connus (après étalonnage complet)
bool homed[MAX_ACTUATORS];               // zéro connu/posé
bool homing[MAX_ACTUATORS];              // homing en cours

// ================== Paramètres de vitesse/homing lents =============
// Un pas toutes les N microsecondes pendant la recherche MIN (par axe)
static uint32_t g_stepIntervalUs = 3000UL; // 3 ms -> ~333 steps/s (doux)
static uint8_t  g_stepSize       = 1;      // 1 pas à la fois pour une approche fine

// ================== FSM par axe optionnel (M5/M6) ==================
// HomingState (HS_IDLE, HS_CHECK…) défini dans Actuator.h via Globals.h
using HS = HomingState;
static HS       hs[2]         = { HS_IDLE, HS_IDLE }; // 0->M5 (idx 4), 1->M6 (idx 5)
static uint32_t lastStepUs[2] = { 0, 0 };             // cadence (par axe)
// Additional retreat after leaving MIN endstop (per optional axis).
static uint16_t releaseBackoffRemain[2] = { 0, 0 };

// Garde de distance avant d'accepter la butée MIN (comme GUARD_MIN_STEPS dans detectMin)
#ifndef HOMING_GUARD_MIN_STEPS
#define HOMING_GUARD_MIN_STEPS 50   // nombre minimal de pas vers le MIN avant d'autoriser la détection
#endif

// Nombre max de pas vers le MIN avant de déclarer un échec (pas d'endstop câblé)
// 65535 pas = course complète possible si HS_SEEK démarre à 65535.
#ifndef HOMING_MAX_SEEK_STEPS
#define HOMING_MAX_SEEK_STEPS 65535U
#endif

// Distance parcourue vers le MIN depuis le début du HS_SEEK (en "steps")
static uint16_t homingMoved[2] = { 0, 0 }; // 0->M5, 1->M6
// Endstop detection arming: only true after we have observed a stable OFF state.
static bool minDetectArmed[2] = { false, false };

static inline uint8_t k2motor(uint8_t k) { return (k==0) ? 5 : 6; } // 0->5, 1->6
static inline uint8_t k2idx  (uint8_t k) { return (k==0) ? 4 : 5; } // 0->4, 1->5

// Planifie au plus un petit incr/decr de target si:
//  - le pas précédent est “consommé” (mTarget == mPosition)
//  - l'intervalle minimal est écoulé
static inline bool scheduleStep(uint8_t k, int8_t dir, uint32_t intervalOverrideUs = 0) {
  const uint8_t i = k2idx(k);
  if (mTarget[i] != mPosition[i]) return false;     // attendre consommation du pas courant
  const uint32_t now = micros();
  const uint32_t stepIntervalUs = intervalOverrideUs ? intervalOverrideUs : g_stepIntervalUs;
  if ((uint32_t)(now - lastStepUs[k]) < stepIntervalUs) return false;

  int32_t next = (int32_t)mPosition[i] + (int32_t)dir * (int32_t)g_stepSize;
  if (next < 0)       next = 0;
  if (next > 65535L)  next = 65535L;

  mTarget[i]    = (uint16_t)next;
  lastStepUs[k] = now;

  // Nouveau : on compte uniquement les pas vers le MIN (dir < 0)
  if (dir < 0 && homingMoved[k] <= (uint16_t)(65535U - g_stepSize)) {
    homingMoved[k] = (uint16_t)(homingMoved[k] + g_stepSize);
  }

  return true;
}

static inline bool hsIsActive(HS s){
  return (s==HS_CHECK || s==HS_BACKOFF || s==HS_SEEK || s==HS_RELEASE);
}

static inline uint32_t hsBackoffIntervalUs() {
  // Backoff can run faster than seek while still staying smooth.
  if (g_stepIntervalUs <= 300UL) return g_stepIntervalUs;
  uint32_t v = g_stepIntervalUs / 3UL;
  return (v < 100UL) ? 100UL : v;
}

// Endstop filtering for homing: keep robust checks at low speed, but
// reduce sampling overhead at high speed so HOMING_S remains effective.
static inline bool hsReadEndstop(uint8_t motor) {
  if (g_stepIntervalUs <= 200UL) {
    return hwReadEndstopStable(motor, 0, 2, 2);
  }
  if (g_stepIntervalUs <= 500UL) {
    return hwReadEndstopStable(motor, 0, 3, 2);
  }
  return hwReadEndstopStable(motor, 0, 5, 3);
}

// ================== API vitesse pas-à-pas ==================
void setMinCalibSpeed(uint32_t interval_us, uint8_t step_size) {
  if (interval_us < 100UL) interval_us = 100UL; // éviter trop rapide
  if (step_size  == 0)     step_size   = 1;
  g_stepIntervalUs = interval_us;
  g_stepSize       = step_size;
}

uint32_t getMinCalibStepIntervalUs() { return g_stepIntervalUs; }

uint16_t getMinCalibStepsPerSecond() {
  if (g_stepIntervalUs == 0) return 0;
  uint32_t sps = (1000000UL + (g_stepIntervalUs/2)) / g_stepIntervalUs; // arrondi
  if (sps > 65535UL) sps = 65535UL;
  return (uint16_t)sps;
}

uint8_t getMinCalibStepSize() { return g_stepSize; }

// ================== Homing MIN non bloquant M5 / M6 ==================

// Lance le homing MIN non bloquant (lent) pour M5/M6.
// doM5 / doM6 : true pour (re)lancer l'axe correspondant.
void startMinCalibration(bool need5, bool need6){
  // clear any previous abort request when starting a fresh calibration
  calibAbortClear();
  const bool do5 = need5 && !homed[4];   // M5 only if not homed yet
  const bool do6 = need6 && !homed[5];   // M6 only if not homed yet

  // Start at mid-range by default so BACKOFF can always move away from MIN
  // if the endstop is already active. When endstop is NOT active, HS_CHECK
  // will switch to a full-range SEEK starting from 65535.
  mPosition[4] = 32768;
  mPosition[5] = 32768;
  mTarget[4]   = 32768;
  mTarget[5]   = 32768;

  hs[0] = do5 ? HS_CHECK : HS_IDLE;      // k=0 -> M5 (i=4)
  hs[1] = do6 ? HS_CHECK : HS_IDLE;      // k=1 -> M6 (i=5)

  homingMoved[0] = 0;
  homingMoved[1] = 0;
  releaseBackoffRemain[0] = 0;
  releaseBackoffRemain[1] = 0;
  minDetectArmed[0] = false;
  minDetectArmed[1] = false;

  if (do5) { homing[4] = true;  homed[4] = false; }
  if (do6) { homing[5] = true;  homed[5] = false; }

  if (do5 || do6) hwLedSet("orange");
}

// Annule toute calibration en cours pour M5/M6.
void cancelMinCalibration(){
  hs[0] = HS_IDLE;
  hs[1] = HS_IDLE;
  homing[4] = false;
  homing[5] = false;
  homed[4]  = false;
  homed[5]  = false;

  mPosition[4] = 65535;
  mPosition[5] = 65535;
  mTarget[4]   = 65535;
  mTarget[5]   = 65535;

  homingMoved[0] = 0;
  homingMoved[1] = 0;
  releaseBackoffRemain[0] = 0;
  releaseBackoffRemain[1] = 0;
  minDetectArmed[0] = false;
  minDetectArmed[1] = false;
}

// Helpers d’état optionnels (si tu veux les utiliser ailleurs)
bool isHomingIdx(uint8_t i) {
  if (i >= MAX_ACTUATORS) return false;
  return homing[i];
}

bool isMinCalibrating(uint8_t motor) { // motor = 5 ou 6
  if (motor < 5 || motor > 6) return false;
  uint8_t idx = (motor == 5) ? 4 : 5;
  return homing[idx];
}

bool isMinCalibrated(uint8_t motor) { // motor = 5 ou 6
  if (motor < 5 || motor > 6) return false;
  uint8_t idx = (motor == 5) ? 4 : 5;
  return homed[idx];
}

// À appeler très souvent (loop). Planifie des pas lents vers le MIN
// pour les axes en calibration sans bloquer la comm ni M1–M4.
void tickMinCalibration() {
  for (uint8_t k = 0; k < 2; k++) {
    HS& st = hs[k];
    if (!hsIsActive(st)) continue;

    // Tant que SimHub n'est pas connecté, on coupe le servo
    // (quand SimHub se connecte et envoie 'A', les servos sont allumés)
    if (!hostConnected) {
      disableServo();
    }

    const uint8_t i = (k==0) ? 4 : 5;   // index tableaux: M5->4, M6->5
    const uint8_t m = (k==0) ? 5 : 6;   // numéro moteur pour endstop: 5/6

    // Sens de recherche et de dégagement selon la position de l'endstop.
    // Direction EFFECTIVE (shared/HomingDir.h) : homing MAX exige un max
    // calibre, sinon repli sur un homing MIN classique.
    const bool   toMax      = HomingDir_EffectiveToMax(axCfg[i].hometoMax, max[i]);
    const int8_t seekDir    = toMax ? +1 : -1;                 // vers endstop
    const int8_t releaseDir = -seekDir;                        // s'en éloigner

    switch (st) {
      case HS_CHECK:
        homingMoved[k] = 0;
        if (hsReadEndstop(m)) {
          // Endstop already active: mid-range so BACKOFF has room in releaseDir.
          mPosition[i] = 32768;
          mTarget[i]   = 32768;
          minDetectArmed[k] = false;
          st = HS_BACKOFF;
        } else {
          // Pas sur l'endstop : partir de l'extrémité opposée pour couvrir toute la course.
          const uint16_t startPos = toMax ? 0 : 65535;
          mPosition[i] = startPos;
          mTarget[i]   = startPos;
          minDetectArmed[k] = true;
          st = HS_SEEK;
        }
        break;

      case HS_BACKOFF:
        // S'éloigner de l'endstop (releaseDir) jusqu'à lecture OFF stable
        if (!hsReadEndstop(m)) {
          minDetectArmed[k] = true;
          homingMoved[k] = 0;
          st = HS_SEEK;
          break;
        }
        scheduleStep(k, releaseDir, hsBackoffIntervalUs());
        break;

      case HS_SEEK: {
        bool minHit = minDetectArmed[k] && hsReadEndstop(m);

        if (minHit) {
          // Keep a short retreat after switch release so the actuator does not
          // stay pressed against the mechanical minimum right after SH_START.
          releaseBackoffRemain[k] = margin[i] ? margin[i] : 20;
          st = HS_RELEASE;
          break;
        }

        // Échec 1 : limite de recherche atteinte sans trouver l'endstop.
        if (homingMoved[k] >= HOMING_MAX_SEEK_STEPS) {
          // Abandon propre : moteur arrêté, homing marqué échoué
          mTarget[i]  = mPosition[i];  // stop immédiat
          homing[i]   = false;
          homed[i]    = false;
          st          = HS_ERROR;
          refreshConnectionLed();
          Serial.print(F("[HOMING] M"));
          Serial.print((unsigned)m);
          Serial.print(F(" aborted: max seek steps reached (moved="));
          Serial.print((unsigned)homingMoved[k]);
          Serial.print(F(", limit="));
          Serial.print((unsigned)HOMING_MAX_SEEK_STEPS);
          Serial.println(F(")"));
          hwLog(F("[HOMING] No endstop found — homing aborted (max seek limit reached; check wiring or disable M5/M6 in Motion Center)"));
          break;
        }

        // Échec 2 : extrémité logique atteinte sans front endstop valide
        // (0 en homing MIN, 65535 en homing MAX).
        if (mPosition[i] == (toMax ? 65535 : 0)) {
          mTarget[i]  = mPosition[i];  // stop immédiat
          homing[i]   = false;
          homed[i]    = false;
          st          = HS_ERROR;
          refreshConnectionLed();
          Serial.print(F("[HOMING] M"));
          Serial.print((unsigned)m);
          Serial.println(F(" aborted: reached logical position 0 without endstop trigger"));
          hwLog(F("[HOMING] No endstop found — homing aborted (position reached 0 without endstop; check wiring or disable M5/M6 in Motion Center)"));
          break;
        }

        // Sinon on avance vers l'endstop
        scheduleStep(k, seekDir);
        break;
      }

      case HS_RELEASE:
        // On se décolle de l'endstop (releaseDir) jusqu'à lecture OFF stable
        if (hsReadEndstop(m)) {
          scheduleStep(k, releaseDir);
          break;
        }

        if (releaseBackoffRemain[k] > 0) {
          if (scheduleStep(k, releaseDir)) {
            const uint16_t dec = (releaseBackoffRemain[k] >= g_stepSize) ? g_stepSize : releaseBackoffRemain[k];
            releaseBackoffRemain[k] = (uint16_t)(releaseBackoffRemain[k] - dec);
          }
          break;
        }

        // Wait until the last scheduled step is consumed before zeroing.
        if (mTarget[i] != mPosition[i]) {
          break;
        }

        if (!hsReadEndstop(m)) {
          // Position de repos directionnelle (shared/HomingDir.h) :
          //  - homing MIN : 0 ;
          //  - homing MAX : max - 2*margin (haut de la plage mappee).
          const uint16_t rest = HomingDir_RestPos(toMax, (uint16_t)max[i], margin[i]);
          mPosition[i] = rest;
          mTarget[i]   = rest;
          homed[i]     = true;
          homing[i]    = false;
          st           = HS_IDLE;   // ← sort proprement, pas de relance
          hwLedSet("red");          // rouge = homing OK (à adapter si tu préfères)
          break;
        }
        // On s'éloigne encore un peu (releaseDir)
        scheduleStep(k, releaseDir);
        break;

      default:
        break;
    }
  }
}


/*
#include "MinCalibration.h" 
#include "HardwareAbstraction.h" 
#include "MotorControl.h" 
#include "Globals.h" 
#include <Arduino.h> 
#include "MinCalibration.h" 
#include "HardwareAbstraction.h" 
#include "CalibrationUtils.h"
volatile bool g_abortRequested = false;

// ================== Nouveaux états clairs ==================
extern bool mConnected[MAX_ACTUATORS];   // défini ailleurs (détection, config…)
extern bool calibrated[MAX_ACTUATORS];  // min/max connus (après étalonnage complet)
bool homed[MAX_ACTUATORS];       // zéro connu/posé
bool homing[MAX_ACTUATORS];      // homing en cours

// ================== Paramètres de vitesse/homing lents =============
// Un pas toutes les N microsecondes pendant la recherche MIN (par axe)
static uint32_t g_stepIntervalUs = 3000UL; // 3 ms -> ~333 steps/s (doux)
static uint8_t  g_stepSize       = 1;      // 1 pas à la fois pour une approche fine

// ================== FSM par axe optionnel (M5/M6) ==================
enum HS : uint8_t { HS_IDLE=0, HS_CHECK, HS_BACKOFF, HS_SEEK, HS_RELEASE, HS_DONE, HS_ERROR };
static HS       hs[2]         = { HS_IDLE, HS_IDLE }; // 0->M5 (idx 4), 1->M6 (idx 5)
static uint32_t lastStepUs[2] = { 0, 0 };             // cadence (par axe)

static inline uint8_t k2motor(uint8_t k) { return (k==0) ? 5 : 6; } // 0->5, 1->6
static inline uint8_t k2idx  (uint8_t k) { return (k==0) ? 4 : 5; } // 0->4, 1->5

// Planifie au plus un petit incr/decr de target si:
//  - le pas précédent est “consommé” (mTarget == mPosition)
//  - l'intervalle minimal est écoulé
static inline void scheduleStep(uint8_t k, int8_t dir) {
  const uint8_t i = k2idx(k);
  if (mTarget[i] != mPosition[i]) return;     // attendre consommation du pas courant
  const uint32_t now = micros();
  if ((uint32_t)(now - lastStepUs[k]) < g_stepIntervalUs) return;

  int32_t next = (int32_t)mPosition[i] + (int32_t)dir * (int32_t)g_stepSize;
  if (next < 0)       next = 0;
  if (next > 65535L)  next = 65535L;
  mTarget[i]   = (uint16_t)next;
  lastStepUs[k]= now;
}

static inline bool hsIsActive(HS s){
  return (s==HS_CHECK || s==HS_BACKOFF || s==HS_SEEK || s==HS_RELEASE);
}

// ================== API vitesse pas-à-pas ==================
void setMinCalibSpeed(uint32_t interval_us, uint8_t step_size) {
  if (interval_us < 100UL) interval_us = 100UL; // éviter trop rapide
  if (step_size  == 0)     step_size   = 1;
  g_stepIntervalUs = interval_us;
  g_stepSize       = step_size;
}

uint32_t getMinCalibStepIntervalUs() { return g_stepIntervalUs; }

uint16_t getMinCalibStepsPerSecond() {
  if (g_stepIntervalUs == 0) return 0;
  uint32_t sps = (1000000UL + (g_stepIntervalUs/2)) / g_stepIntervalUs; // arrondi
  if (sps > 65535UL) sps = 65535UL;
  return (uint16_t)sps;
}

uint8_t getMinCalibStepSize() { return g_stepSize; }
// === Ajoute ce mask en haut du fichier ===

// --- startMinCalibration: arme le mask selon les axes demandés ---
void startMinCalibration(bool need5, bool need6){
  const bool do5 = need5 && !homed[4];   // M5 only if not homed yet
  const bool do6 = need6 && !homed[5];   // M6 only if not homed yet
  mPosition[4] = 65535;
  mPosition[5] = 65535;
  mTarget[4] = 65535;
  mTarget[5] = 65535;
  hs[0] = do5 ? HS_CHECK : HS_IDLE;      // k=0 -> M5 (i=4)
  hs[1] = do6 ? HS_CHECK : HS_IDLE;      // k=1 -> M6 (i=5)

  if (do5) { homing[4] = true;  homed[4] = false; }
  if (do6) { homing[5] = true;  homed[5] = false; }

  if (do5 || do6) hwLedSet("orange");
}


void cancelMinCalibration(){
  hs[0] = HS_IDLE;
  hs[1] = HS_IDLE;
  homing[4] = false;
  homing[5] = false;
  homed[4] = false;
  homed[5] = false;
  mPosition[4] = 65535;
  mPosition[5] = 65535;
  mTarget[4] = 65535;
  mTarget[5] = 65535;
}
void tickMinCalibration() {
  for (uint8_t k=0; k<2; k++) {
    HS& st = hs[k];
    if (!hsIsActive(st)) continue;
if(!hostConnected) disableServo();
    const uint8_t i = (k==0) ? 4 : 5;   // index tableaux: M5->4, M6->5
    const uint8_t m = (k==0) ? 5 : 6;   // numéro moteur pour endstop: 5/6

    switch (st) {
      case HS_CHECK:
        st = hwReadEndstopStable(m, 0, 5, 3) ? HS_BACKOFF : HS_SEEK;
        break;

      case HS_BACKOFF:
        if (!hwReadEndstopStable(m, 0, 5, 3)) { st = HS_SEEK; break; }
        scheduleStep(k, +1);
        break;

      case HS_SEEK:
      
        if (hwReadEndstopStable(m, 0, 5, 3)) { st = HS_RELEASE; break; }
        if (mPosition[i] == 0) { scheduleStep(k, +1); break; }
  
        scheduleStep(k, -1);
        break;

      case HS_RELEASE:
        if (!hwReadEndstopStable(m, 0, 5, 3)) {
          mPosition[i] = 0;
          mTarget[i]   = 0;
          homed[i]     = true;
          homing[i]    = false;
          st           = HS_IDLE;   // ← sort proprement, pas de relance
          hwLedSet("red");
          break;
        }
        scheduleStep(k, +1);
        break;

      default: break;
    }
  }

}
*/
