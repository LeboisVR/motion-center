#include "Homing.h"
#include "Actuator.h"
#include "HardwareAbstraction.h"
#include "MotorControl.h"
#include "Globals.h"
#include <Arduino.h>

volatile bool g_homingAbort = false;

// Nombre max de pas vers le MIN avant de déclarer un échec
#ifndef HOMING_MAX_SEEK_STEPS
  #define HOMING_MAX_SEEK_STEPS 65535U
#endif

// ── Paramètres globaux de vitesse ─────────────────────────────────────────
static uint32_t g_intervalUs = 3000UL;   // µs entre deux pas (~333 steps/s)
static uint8_t  g_stepSize   = 1;

void homingSetSpeed(uint32_t interval_us, uint8_t step_size) {
    if (interval_us < 100UL) interval_us = 100UL;
    if (step_size   == 0)    step_size   = 1;
    g_intervalUs = interval_us;
    g_stepSize   = step_size;
}

uint32_t homingGetIntervalUs() { return g_intervalUs; }

uint16_t homingGetSps() {
    if (g_intervalUs == 0) return 0;
    // TODO: (1000000UL + g_intervalUs/2) / g_intervalUs  (arrondi)
    return 0;
}

uint8_t homingGetStepSize() { return g_stepSize; }

// ── Helpers internes ──────────────────────────────────────────────────────

// Lit et filtre l'endstop de l'axe idx.
// idx : index dans ax[] (0-based)  →  motor = idx + 1
static bool readEndstop(uint8_t idx) {
    // TODO: appeler hwReadEndstopStable(motor, 0, samples, minOn)
    //       adapter le nb de samples selon g_intervalUs
    //         ≤200 µs → (motor, 0, 2, 2)
    //         ≤500 µs → (motor, 0, 3, 2)
    //         sinon   → (motor, 0, 5, 3)
    (void)idx;
    return false;
}

// Intervalle accéléré pour la phase BACKOFF (recul plus rapide que la recherche)
static uint32_t backoffIntervalUs() {
    // TODO: g_intervalUs / 3, clamp min 100 µs
    //       si g_intervalUs <= 300 → retourner g_intervalUs tel quel
    return g_intervalUs;
}

// ── FSM d'un axe ──────────────────────────────────────────────────────────
static void tickAxis(uint8_t idx) {
    ActuatorState&        s   = ax[idx];
    const ActuatorConfig& cfg = axCfg[idx];
    (void)cfg;   // utilisé dans HS_SEEK et HS_RELEASE une fois les TODO remplis

    switch (s.hs) {

        case HS_CHECK:
            s.moved = 0;
            if (readEndstop(idx)) {
                // Endstop déjà actif : partir du milieu pour avoir de la marge en BACKOFF
                // TODO: s.pos    = 32768
                // TODO: s.target = 32768
                // TODO: s.armed  = false
                // TODO: s.hs     = HS_BACKOFF
            } else {
                // Endstop inactif : partir du max pour couvrir toute la course
                // TODO: s.pos    = 65535
                // TODO: s.target = 65535
                // TODO: s.armed  = true
                // TODO: s.hs     = HS_SEEK
            }
            break;

        case HS_BACKOFF:
            if (!readEndstop(idx)) {
                // OFF stable atteint → armer la détection et démarrer la recherche
                // TODO: s.armed  = true
                // TODO: s.moved  = 0
                // TODO: s.hs     = HS_SEEK
                break;
            }
            // TODO: axStep(idx, +1, backoffIntervalUs(), g_stepSize)
            break;

        case HS_SEEK: {
            if (s.armed && readEndstop(idx)) {
                // Endstop trouvé → phase de dégagement
                // TODO: s.backoffLeft = cfg.margin ? cfg.margin : 20
                // TODO: s.hs          = HS_RELEASE
                break;
            }

            // Échec 1 : trop de pas parcourus sans trouver l'endstop
            if (s.moved >= HOMING_MAX_SEEK_STEPS) {
                // TODO: s.target = s.pos   (stop immédiat)
                // TODO: s.homing = false
                // TODO: s.homed  = false
                // TODO: s.hs     = HS_ERROR
                // TODO: refreshConnectionLed()
                // TODO: Serial.print / hwLog → message d'erreur
                break;
            }

            // Échec 2 : position logique 0 atteinte sans trigger endstop
            if (s.pos == 0) {
                // TODO: s.target = s.pos
                // TODO: s.homing = false
                // TODO: s.homed  = false
                // TODO: s.hs     = HS_ERROR
                // TODO: refreshConnectionLed()
                // TODO: Serial.print / hwLog → message d'erreur
                break;
            }

            // TODO: axStep(idx, -1, g_intervalUs, g_stepSize)
            break;
        }

        case HS_RELEASE:
            if (readEndstop(idx)) {
                // Encore sur la butée → continuer à reculer
                // TODO: axStep(idx, +1, g_intervalUs, g_stepSize)
                break;
            }

            if (s.backoffLeft > 0) {
                // Recul de sécurité après relâchement du contact
                // TODO: si axStep(idx, +1, g_intervalUs, g_stepSize) → décrémenter s.backoffLeft de g_stepSize (clamp 0)
                break;
            }

            // Attendre que le dernier pas soit consommé
            // TODO: si s.target != s.pos → break

            // Zeroing
            // TODO: s.pos        = 0
            // TODO: s.target     = 0
            // TODO: s.homed      = true
            // TODO: s.homing     = false
            // TODO: s.hs         = HS_IDLE
            // TODO: hwLedSet("red")
            break;

        case HS_ERROR:
            // Rester en erreur jusqu'à homingCancel() ou nouveau homingStart()
            break;

        default:
            break;
    }
}

// ── API publique ──────────────────────────────────────────────────────────

void homingStart(uint8_t idx_mask) {
    homingAbortClear();
    bool anyStarted = false;

    for (uint8_t i = 0; i < MAX_ACTUATORS; i++) {
        if (!(idx_mask & (1u << i)))  continue;
        if (!axCfg[i].connected)      continue;
        if (!axCfg[i].hasEndstop)     continue;
        // V1/V2 : seuls M5/M6 (idx 4-5) ont leurs butées accessibles par le firmware.
        // M1-M4 ont des butées mécaniques mais l'état ne remonte pas au firmware.
        if (boxCfg.version <= 2 && i < 4) continue;
        if (ax[i].homed)              continue;

        // TODO: ax[i].pos         = 32768
        // TODO: ax[i].target      = 32768
        // TODO: ax[i].hs          = HS_CHECK
        // TODO: ax[i].homing      = true
        // TODO: ax[i].homed       = false
        // TODO: ax[i].moved       = 0
        // TODO: ax[i].backoffLeft = 0
        // TODO: ax[i].armed       = false
        // TODO: ax[i].lastStepUs  = 0
        anyStarted = true;
    }

    if (anyStarted) {
        // TODO: hwLedSet("orange")
    }
}

void homingCancel() {
    for (uint8_t i = 0; i < MAX_ACTUATORS; i++) {
        if (!ax[i].homing) continue;
        // TODO: ax[i].target = ax[i].pos   (stop immédiat)
        // TODO: ax[i].homing = false
        // TODO: ax[i].homed  = false
        // TODO: ax[i].hs     = HS_IDLE
    }
    // TODO: refreshConnectionLed()
}

void homingTick() {
    if (!hostConnected) { disableServo(); }
    if (g_homingAbort)  { homingCancel(); homingAbortClear(); return; }

    for (uint8_t i = 0; i < MAX_ACTUATORS; i++) {
        if (ax[i].homing) tickAxis(i);
    }
}

bool homingIsActive(uint8_t i) { return i < MAX_ACTUATORS && ax[i].homing; }
bool homingIsDone  (uint8_t i) { return i < MAX_ACTUATORS && ax[i].homed;  }
bool homingIsError (uint8_t i) { return i < MAX_ACTUATORS && ax[i].hs == HS_ERROR; }
