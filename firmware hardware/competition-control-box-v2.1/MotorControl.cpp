/*
 * MotorControl.cpp
 *
 * Code version 1.1 — Timer3 metronome ISR (architecture alignée sur le G0B1)
 *
 * Implements the motor control functions declared in MotorControl.h.  This
 * module encapsulates the low-level stepper control logic used by the
 * competition control box.  Step and direction pin assignments reside here
 * rather than scattered throughout the main sketch.
 *
 * ---------------------------------------------------------------------------
 * CHANGEMENT D'ARCHITECTURE (v1.1)
 * ---------------------------------------------------------------------------
 * Avant : bit-bang depuis loop(), un pas par passage, cadence esclave de la
 * frequence de la boucle principale (donc du cout de SerialReaderP / USB CDC).
 *
 * Maintenant : ISR metronome sur Timer3 (CTC, STEP_TICK_HZ), tous les axes
 * pulses atomiquement dans la meme ISR — le meme modele que le firmware
 * STM32G0B1 (TIM14 metronome, single-ISR bitbang). Consequences :
 *
 *   - Cadence de pas deterministe, totalement decouplee de l'USB.
 *     loop() peut passer 100 % de son temps dans SerialReaderP().
 *   - Plus de delayMicroseconds(directionDelay) dans le chemin chaud :
 *     sur inversion de sens, on pose DIR et on SAUTE le pulse de ce tick.
 *     Le setup DIR vaut donc 1 tick complet (>= 50 us @ 20 kHz), tres
 *     au-dela des 8 us requis par les optos A6-RS/AASD.
 *   - Impulsion STEP pleine largeur : STEP passe HIGH en fin de tick N et
 *     repasse LOW en tout debut de tick N+1. Largeur ~1 tick, ~50 % de
 *     rapport cyclique — confortable pour les optocoupleurs (spec >= 2.5 us),
 *     la ou l'ancien HIGH->LOW dos a dos faisait ~150-300 ns.
 *
 * REGLES D'ATOMICITE (AVR 8 bits, valeurs 16 bits) :
 *   - Toute ECRITURE de mTarget[i] depuis le contexte main (parser serie)
 *     doit etre atomique : utiliser motorSetTarget() ci-dessous, ou envelopper
 *     dans ATOMIC_BLOCK(ATOMIC_RESTORESTATE).
 *   - Toute LECTURE de mPosition[i] depuis le contexte main (status, GET)
 *     doit etre atomique : utiliser motorGetPosition().
 *   Sans cela, l'ISR peut lire une cible a moitie ecrite (octet haut/bas
 *   incoherents) et partir 256 pas a cote — meme famille de symptomes que
 *   le drift historique observe sur G0B1.
 *
 * A DECLARER DANS MotorControl.h :
 *   void     motorSetTarget(uint8_t idx, uint16_t v);
 *   uint16_t motorGetPosition(uint8_t idx);
 *
 * A FAIRE DANS LE SKETCH PRINCIPAL :
 *   - setup() : appeler motorTimerInit() APRES motorInit().
 *   - loop()  : l'appel a moveMotor() peut etre supprime (il devient un
 *     no-op tant que l'ISR est active) ; SerialReaderP() tourne librement.
 *   - SerialReaderP : remplacer les ecritures directes mTarget[i] = v par
 *     motorSetTarget(i, v).
 * ---------------------------------------------------------------------------
 */

#include "MotorControl.h"
#include "HardwareAbstraction.h"
#include "CalibrationUtils.h"
#include <Arduino.h>
#include "Globals.h"
#include "MinCalibration.h"
#include <util/atomic.h>

// Define step and direction pins.  Adjust the order to match your wiring.
// These arrays must be kept in sync with the MAX_ACTUATORS define.  Only the
// first Actuators_Count entries (set in the main sketch) are actively used.
//
// Do **not** mark these arrays as const.  In C++ const variables at
// namespace scope have internal linkage unless there is an extern
// declaration with the same type.  Other modules refer to StepPins and
// DirPins via extern declarations in MotorControl.h, so dropping the
// const qualifier ensures the symbols are emitted with external linkage.
#if MAX_ACTUATORS > 6
uint8_t StepPins[MAX_ACTUATORS] = {8, 9, 10, 11, 12, A2, A4}; // M1-M7
uint8_t DirPins [MAX_ACTUATORS] = {4, 5, 6, 7, 3,  A3, A5};   // M1-M7
#elif MAX_ACTUATORS > 5
uint8_t StepPins[MAX_ACTUATORS] = {8, 9, 10, 11, 12, A2}; // M1-M6 (Box V2)
uint8_t DirPins [MAX_ACTUATORS] = {4, 5, 6, 7, 3,  A3};   // M1-M6 (Box V2)
#else
uint8_t StepPins[MAX_ACTUATORS] = {8, 9, 10, 11, 12};     // M1-M5 (Box V1)
uint8_t DirPins [MAX_ACTUATORS] = {4, 5, 6, 7, 3};        // M1-M5 (Box V1)
#endif

// directionDelay ne sert plus au chemin chaud (le skip-tick DIR le remplace),
// mais reste utilise par le chemin de calibration bit-bang (moveMotor()).
uint8_t directionDelay = 8;   // us de setup DIR avant STEP (reference F103 : 8 us pour l'opto A6-RS/AASD)
bool servoEnabled    = false;

#define powerpin 15   // sck pin

// Global actuator state.  Each array entry corresponds to a motor.
// mPosition[i] / mTarget[i] -> definis comme shims dans Globals.h -> ax[i].pos / ax[i].target
#if MAX_ACTUATORS > 5
extern  int8_t   actuatorDir     [MAX_ACTUATORS] = {-1, -1, -1, -1, -1, -1};
#else
extern  int8_t   actuatorDir     [MAX_ACTUATORS] = {-1, -1, -1, -1, -1};
#endif

// The servo power is switched via a relay.  Relay control is handled
// through the hardware abstraction (hwRelayInit/hwRelayOn/hwRelayOff).
void motorInit() {
  // Configure all step/dir pins.  Only first N motors are actively used but
  // configure all defined pins to be safe.
  // Configure the relay controlling the servo power and ensure it is off.
  hwRelayInit();
  hwInitCalibrationPins();
  updateActiveActuatorCount();
  for (uint8_t m = 1; m <= MAX_ACTUATORS; m++) {
    loadCalibration(m);
  }

  for (uint8_t i = 0; i < MAX_ACTUATORS; i++) {
    pinMode(StepPins[i], OUTPUT);
    pinMode(DirPins[i], OUTPUT);
    digitalWrite(DirPins[i], HIGH);
  }
  pinMode(powerpin, OUTPUT);
  // Disable the servo power until explicitly enabled.
  disableServo();
  homed[4]=false;
#if MAX_ACTUATORS > 5
  homed[5]=false;
#endif
#if MAX_ACTUATORS > 6
  homed[6]=false;
#endif
}

uint8_t activeActuatorCount = 4;  // pas besoin de volatile ici
void updateActiveActuatorCount() {
#if MAX_ACTUATORS > 6
  activeActuatorCount = mConnected[6] ? 7 : (mConnected[5] ? 6 : (mConnected[4] ? 5 : 4));
#elif MAX_ACTUATORS > 5
  activeActuatorCount = mConnected[5] ? 6 : (mConnected[4] ? 5 : 4);
#else
  activeActuatorCount = mConnected[4] ? 5 : 4;
#endif
}

// ---------------------------------------------------------------------------
// Acces atomiques cible/position (contexte main <-> ISR).
// Sur AVR, un uint16_t n'est ni lu ni ecrit atomiquement : sans ces
// enveloppes, l'ISR peut observer un mot a moitie mis a jour.
// ---------------------------------------------------------------------------
// Atomic helpers moved to Globals.h (atomicSetTarget/atomicGetPosition)
// Keep no duplicate implementations here to avoid multiple definitions.

#if defined(TARGET_LEONARDO)

#include <avr/interrupt.h>

// ---------------------------------------------------------------------------
// ISR metronome Timer3 — un tick = une opportunite de pas pour chaque axe.
//
// Timer3, mode CTC (WGM32), prescaler /8 -> 2 MHz de base de temps.
// OCR3A = (F_CPU / 8 / STEP_TICK_HZ) - 1.
//
//   STEP_TICK_HZ = 30000 -> OCR3A = 66 @ 16 MHz, tick de 33.3 us, 30 kSPS max
//   par axe. Budget ISR mesurable : ~15-20 us pour 6 axes en pas -> ~30-40 %
//   de CPU, le reste pour l'USB (les IRQ USB tolerent largement 20 us de
//   latence). Au-dela de ~25 kSPS c'est le 32U4 qui plafonne.
//
// Sequencement d'un tick :
//   1. Fin d'impulsion : les STEP montes au tick precedent repassent LOW.
//      -> largeur d'impulsion HIGH = 1 tick complet (~50 us), LOW minimal =
//         duree du calcul des axes (~15 us), tous deux >> 2.5 us requis.
//   2. Pour chaque axe hors cible :
//        - si le sens demande != sens courant : poser DIR, memoriser le
//          nouveau sens, NE PAS pulser ce tick (skip-tick = setup DIR d'un
//          tick complet, remplace delayMicroseconds(directionDelay)).
//        - sinon : armer le bit STEP et incrementer la position.
//   3. Monter tous les STEP armes simultanement (ecritures port groupees,
//      inter-axes atomique comme sur le G0B1).
// ---------------------------------------------------------------------------

#ifndef STEP_TICK_HZ
#define STEP_TICK_HZ 30000UL
#endif

static bool g_calibMotionActive = false;

// Masques des STEP restes HIGH a la fin du tick precedent (a rabaisser).
// Ecrits et lus uniquement sous ISR ou IRQ masquees -> pas besoin de volatile,
// mais on le garde pour l'hygiene (acces aussi depuis stopMotionTimer()).
static volatile uint8_t g_pendB = 0;
static volatile uint8_t g_pendD = 0;
static volatile uint8_t g_pendF = 0;

static inline void stepPinsAllLow() {
  // Rabaisse toute impulsion en cours (transitions ISR <-> calibration).
  PORTB &= (uint8_t)~(_BV(PB4) | _BV(PB5) | _BV(PB6) | _BV(PB7));
  PORTD &= (uint8_t)~_BV(PD6);
  PORTF &= (uint8_t)~_BV(PF5);
  g_pendB = 0; g_pendD = 0; g_pendF = 0;
}

void motorTimerInit() {
  cli();
  TCCR3A = 0;
  TCCR3B = _BV(WGM32) | _BV(CS31);            // CTC sur OCR3A, prescaler /8
  OCR3A  = (uint16_t)((F_CPU / 8UL / STEP_TICK_HZ) - 1UL);
  TCNT3  = 0;
  TIFR3  = _BV(OCF3A);                        // purge un eventuel flag en attente
  TIMSK3 = _BV(OCIE3A);                       // ISR metronome active
  sei();
}

ISR(TIMER3_COMPA_vect) {
  // --- Phase 1 : fin des impulsions du tick precedent (pleine largeur) ---
  uint8_t p;
  p = g_pendB; if (p) { PORTB &= (uint8_t)~p; g_pendB = 0; }
  p = g_pendD; if (p) { PORTD &= (uint8_t)~p; g_pendD = 0; }
  p = g_pendF; if (p) { PORTF &= (uint8_t)~p; g_pendF = 0; }

  if (!servoEnabled) return;

  // --- Phase 2 : calcul des axes ---
  const uint8_t count = activeActuatorCount;
  uint8_t mB = 0, mD = 0, mF = 0;

  // M1 (DIR PD4, STEP PB4)
  {
    const uint16_t pos = mPosition[0];
    const uint16_t tgt = mTarget[0];
    if (tgt != pos) {
      const int8_t want = (tgt > pos) ? +1 : -1;
      if (want != actuatorDir[0]) {
        if (want > 0) PORTD &= ~_BV(PD4); else PORTD |= _BV(PD4);
        actuatorDir[0] = want;              // skip-tick : DIR pose, pas de pulse
      } else {
        mB |= _BV(PB4);
        mPosition[0] = (uint16_t)(pos + want);
      }
    }
  }

  // M2 (DIR PC6, STEP PB5)
  if (count > 1) {
    const uint16_t pos = mPosition[1];
    const uint16_t tgt = mTarget[1];
    if (tgt != pos) {
      const int8_t want = (tgt > pos) ? +1 : -1;
      if (want != actuatorDir[1]) {
        if (want > 0) PORTC &= ~_BV(PC6); else PORTC |= _BV(PC6);
        actuatorDir[1] = want;
      } else {
        mB |= _BV(PB5);
        mPosition[1] = (uint16_t)(pos + want);
      }
    }
  }

  // M3 (DIR PD7, STEP PB6)
  if (count > 2) {
    const uint16_t pos = mPosition[2];
    const uint16_t tgt = mTarget[2];
    if (tgt != pos) {
      const int8_t want = (tgt > pos) ? +1 : -1;
      if (want != actuatorDir[2]) {
        if (want > 0) PORTD &= ~_BV(PD7); else PORTD |= _BV(PD7);
        actuatorDir[2] = want;
      } else {
        mB |= _BV(PB6);
        mPosition[2] = (uint16_t)(pos + want);
      }
    }
  }

  // M4 (DIR PE6, STEP PB7)
  if (count > 3) {
    const uint16_t pos = mPosition[3];
    const uint16_t tgt = mTarget[3];
    if (tgt != pos) {
      const int8_t want = (tgt > pos) ? +1 : -1;
      if (want != actuatorDir[3]) {
        if (want > 0) PORTE &= ~_BV(PE6); else PORTE |= _BV(PE6);
        actuatorDir[3] = want;
      } else {
        mB |= _BV(PB7);
        mPosition[3] = (uint16_t)(pos + want);
      }
    }
  }

  // M5 (DIR PD0, STEP PD6)
  if (count > 4) {
    const uint16_t pos = mPosition[4];
    const uint16_t tgt = mTarget[4];
    if (tgt != pos) {
      const int8_t want = (tgt > pos) ? +1 : -1;
      if (want != actuatorDir[4]) {
        if (want > 0) PORTD &= ~_BV(PD0); else PORTD |= _BV(PD0);
        actuatorDir[4] = want;
      } else {
        mD |= _BV(PD6);
        mPosition[4] = (uint16_t)(pos + want);
      }
    }
  }

  // M6 (DIR PF4, STEP PF5)
  if (count > 5) {
    const uint16_t pos = mPosition[5];
    const uint16_t tgt = mTarget[5];
    if (tgt != pos) {
      const int8_t want = (tgt > pos) ? +1 : -1;
      if (want != actuatorDir[5]) {
        if (want > 0) PORTF &= ~_BV(PF4); else PORTF |= _BV(PF4);
        actuatorDir[5] = want;
      } else {
        mF |= _BV(PF5);
        mPosition[5] = (uint16_t)(pos + want);
      }
    }
  }

  // --- Phase 3 : front montant simultane, memorise pour le tick suivant ---
  if (mB) { PORTB |= mB; g_pendB = mB; }
  if (mD) { PORTD |= mD; g_pendD = mD; }
  if (mF) { PORTF |= mF; g_pendF = mF; }
}

// ---------------------------------------------------------------------------
// moveMotor() — chemin de CALIBRATION uniquement.
//
// Les boucles de calibration bloquantes (MinCalibration / CalibrationUtils)
// appellent moveMotor() avec leur propre cadence (scheduleStep). Pendant la
// calibration, l'ISR est masquee (enterCalibrationMotionMode) et ce bit-bang
// reprend la main, identique au modele v1.5 : un pas simultane par appel,
// setup DIR par delayMicroseconds.
//
// Garde-fou : si l'ISR metronome est active, moveMotor() est un no-op —
// un appel residuel depuis loop() ne peut pas injecter de pas parasites.
// ---------------------------------------------------------------------------
void moveMotor() {
  if (TIMSK3 & _BV(OCIE3A)) return;   // ISR active : le metronome a l'exclusivite
  if (!servoEnabled) return;

  const uint8_t count = activeActuatorCount;
  uint8_t mB = 0, mD = 0, mF = 0;
  bool dirChanged = false;

  // M1 (DIR PD4, STEP PB4)
  {
    const uint16_t pos = mPosition[0];
    const uint16_t tgt = mTarget[0];
    if (tgt != pos) {
      const int8_t want = (tgt > pos) ? +1 : -1;
      if (want != actuatorDir[0]) {
        if (want > 0) PORTD &= ~_BV(PD4); else PORTD |= _BV(PD4);
        actuatorDir[0] = want;
        dirChanged = true;
      }
      mB |= _BV(PB4);
      mPosition[0] = (uint16_t)(pos + want);
    }
  }

  // M2 (DIR PC6, STEP PB5)
  if (count > 1) {
    const uint16_t pos = mPosition[1];
    const uint16_t tgt = mTarget[1];
    if (tgt != pos) {
      const int8_t want = (tgt > pos) ? +1 : -1;
      if (want != actuatorDir[1]) {
        if (want > 0) PORTC &= ~_BV(PC6); else PORTC |= _BV(PC6);
        actuatorDir[1] = want;
        dirChanged = true;
      }
      mB |= _BV(PB5);
      mPosition[1] = (uint16_t)(pos + want);
    }
  }

  // M3 (DIR PD7, STEP PB6)
  if (count > 2) {
    const uint16_t pos = mPosition[2];
    const uint16_t tgt = mTarget[2];
    if (tgt != pos) {
      const int8_t want = (tgt > pos) ? +1 : -1;
      if (want != actuatorDir[2]) {
        if (want > 0) PORTD &= ~_BV(PD7); else PORTD |= _BV(PD7);
        actuatorDir[2] = want;
        dirChanged = true;
      }
      mB |= _BV(PB6);
      mPosition[2] = (uint16_t)(pos + want);
    }
  }

  // M4 (DIR PE6, STEP PB7)
  if (count > 3) {
    const uint16_t pos = mPosition[3];
    const uint16_t tgt = mTarget[3];
    if (tgt != pos) {
      const int8_t want = (tgt > pos) ? +1 : -1;
      if (want != actuatorDir[3]) {
        if (want > 0) PORTE &= ~_BV(PE6); else PORTE |= _BV(PE6);
        actuatorDir[3] = want;
        dirChanged = true;
      }
      mB |= _BV(PB7);
      mPosition[3] = (uint16_t)(pos + want);
    }
  }

  // M5 (DIR PD0, STEP PD6)
  if (count > 4) {
    const uint16_t pos = mPosition[4];
    const uint16_t tgt = mTarget[4];
    if (tgt != pos) {
      const int8_t want = (tgt > pos) ? +1 : -1;
      if (want != actuatorDir[4]) {
        if (want > 0) PORTD &= ~_BV(PD0); else PORTD |= _BV(PD0);
        actuatorDir[4] = want;
        dirChanged = true;
      }
      mD |= _BV(PD6);
      mPosition[4] = (uint16_t)(pos + want);
    }
  }

  // M6 (DIR PF4, STEP PF5)
  if (count > 5) {
    const uint16_t pos = mPosition[5];
    const uint16_t tgt = mTarget[5];
    if (tgt != pos) {
      const int8_t want = (tgt > pos) ? +1 : -1;
      if (want != actuatorDir[5]) {
        if (want > 0) PORTF &= ~_BV(PF4); else PORTF |= _BV(PF4);
        actuatorDir[5] = want;
        dirChanged = true;
      }
      mF |= _BV(PF5);
      mPosition[5] = (uint16_t)(pos + want);
    }
  }

  if (!(mB | mD | mF)) return;

  // Setup DIR uniquement sur inversion de sens.
  if (dirChanged) delayMicroseconds(directionDelay);

  // STEP HIGH puis LOW immediatement - tous les moteurs simultanement.
  if (mB) PORTB |= mB;
  if (mD) PORTD |= mD;
  if (mF) PORTF |= mF;

  if (mB) PORTB &= (uint8_t)~mB;
  if (mD) PORTD &= (uint8_t)~mD;
  if (mF) PORTF &= (uint8_t)~mF;
}

// ---- Gestion du metronome / mode calibration --------------------------------
// startMotionTimer/stopMotionTimer conservent leur signature historique.
// stopMotionTimer masque l'ISR et rabaisse les STEP ; startMotionTimer
// rearme le metronome a STEP_TICK_HZ (le parametre interval_us historique
// est ignore : la cadence runtime est fixe, la cadence de calibration est
// pilotee par les boucles de calibration elles-memes).

void startMotionTimer(uint16_t interval_us) {
  (void)interval_us;
  TIFR3  = _BV(OCF3A);
  TIMSK3 = _BV(OCIE3A);
}

void stopMotionTimer() {
  TIMSK3 = 0;
  stepPinsAllLow();
}

void enterCalibrationMotionMode() {
  // La calibration reprend le bit-bang par appels moveMotor() : on masque
  // le metronome pour lui rendre l'exclusivite des ports.
  TIMSK3 = 0;
  stepPinsAllLow();
  g_calibMotionActive = true;
}

void exitCalibrationMotionMode() {
  g_calibMotionActive = false;
  // Purge du flag avant rearmement : pas de tick fantome accumule pendant
  // la calibration.
  TIFR3  = _BV(OCF3A);
  TIMSK3 = _BV(OCIE3A);
}

bool isCalibrationMotionModeActive() {
  return g_calibMotionActive;
}

#endif // TARGET_LEONARDO



void enableServo() {
  // Turn on the servo power via the relay
  digitalWrite(powerpin, LOW);
  hwRelayOn();
  servoEnabled = true;
  if(!hostConnected){hwLog(F("WARNING SERVO ENABLED"));}
  hwLedSet("red");  
}

void disableServo() {
  // Turn off the servo power via the relay
  digitalWrite(powerpin, HIGH);
  hwRelayOff();
  servoEnabled = false;
  if(!hostConnected){hwLog(F("WARNING SERVO DISABLED"));}
  syncHostConnectionState();
  refreshConnectionLed();
}

void loadConfig() {
  // Read M5 connection flag
  uint16_t flag5 = hwEepromReadU16(EEPROM_M5CONNECTED_ADDR);
  mConnected[4] = (flag5 != 0);

  // Read M6 connection flag
  uint16_t flag6 = hwEepromReadU16(EEPROM_M6CONNECTED_ADDR);
  mConnected[5] = (flag6 != 0);

  // Direction de homing M5/M6 (0=MIN, 1=MAX). Garde "== 1" : cellule
  // vierge (0xFFFF) => MIN.
  axCfg[4].hometoMax = (hwEepromReadU16(EEPROM_M5HOMINGDIR_ADDR) == 1);
#if MAX_ACTUATORS > 5
  axCfg[5].hometoMax = (hwEepromReadU16(EEPROM_M6HOMINGDIR_ADDR) == 1);
#endif

  // Endpark % (0..100, 255=off). Garde "<= 100" : cellule vierge (0xFFFF)
  // ou ancienne valeur invalide => off (comportement historique conservé).
  for (uint8_t i = 0; i < MAX_ACTUATORS; i++) axCfg[i].endParkPct = ENDPARK_OFF;
  {
    uint16_t ep = hwEepromReadU16(EEPROM_M5ENDPARK_ADDR);
    if (ep <= 100) axCfg[4].endParkPct = (uint8_t)ep;
#if MAX_ACTUATORS > 5
    ep = hwEepromReadU16(EEPROM_M6ENDPARK_ADDR);
    if (ep <= 100) axCfg[5].endParkPct = (uint8_t)ep;
#endif
  }

  uint16_t sps = hwEepromReadU16(EEPROM_HOMINGSPS_ADDR);
if (sps < HOMING_SPS_MIN || sps > HOMING_SPS_MAX) sps = 300;
uint32_t interval_us = (1000000UL + (sps/2)) / sps;
uint8_t step_size = 1;
  if (sps >= 9000U) step_size = 16;
  else if (sps >= 6000U) step_size = 12;
  else if (sps >= 3500U) step_size = 8;
  else if (sps >= 1800U) step_size = 5;
  else if (sps >= 800U)  step_size = 3;
  else if (sps >= 300U)  step_size = 2;
setMinCalibSpeed(interval_us, step_size);
recomputeExpectedBytesP(); 

}
