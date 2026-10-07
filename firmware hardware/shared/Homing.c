// Homing.c — homing MIN non bloquant + calibration complète (M5/M6)
//
// Transcription fidèle de MinCalibration.cpp (AVR, validé terrain) sur le
// modèle partagé ax[]/axCfg[] via axStep(). C pur, portable AVR / STM32.
//
// Différences voulues vs MinCalibration.cpp :
//  - états par axe portés par ax[i] (hs, moved, backoffLeft, armed, lastStepUs)
//    au lieu de tableaux statiques locaux [2] ;
//  - tous les axes avec axCfg[i].hasEndstop sont éligibles (pas seulement
//    M5/M6 en dur) — le filtre V1/V2 reste appliqué via boxCfg.version ;
//  - couche calibration complète (DETECT_MAX / FULL_CALIB) intégrée.

#include "Homing.h"
#include "HomingDir.h"

volatile bool g_homingAbort = false;

// Nombre max de pas vers l'endstop avant de déclarer un échec
#ifndef HOMING_MAX_SEEK_STEPS
  #define HOMING_MAX_SEEK_STEPS 65535U
#endif

// Recul par défaut après relâchement de la butée si margin == 0
#ifndef HOMING_DEFAULT_RELEASE_BACKOFF
  #define HOMING_DEFAULT_RELEASE_BACKOFF 20U
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

uint32_t homingGetIntervalUs(void) { return g_intervalUs; }

uint16_t homingGetSps(void) {
    if (g_intervalUs == 0) return 0;
    uint32_t sps = (1000000UL + (g_intervalUs / 2UL)) / g_intervalUs; // arrondi
    if (sps > 65535UL) sps = 65535UL;
    return (uint16_t)sps;
}

uint8_t homingGetStepSize(void) { return g_stepSize; }

// ── Helpers internes ──────────────────────────────────────────────────────

// Lit et filtre l'endstop de l'axe idx (motor = idx + 1).
// Filtrage adapté à la vitesse, comme hsReadEndstop() de MinCalibration.cpp.
static bool readEndstop(uint8_t idx) {
    const uint8_t motor = (uint8_t)(idx + 1U);
    if (g_intervalUs <= 200UL) return hwReadEndstopStable(motor, 0, 2, 2);
    if (g_intervalUs <= 500UL) return hwReadEndstopStable(motor, 0, 3, 2);
    return hwReadEndstopStable(motor, 0, 5, 3);
}

// Intervalle accéléré pour la phase BACKOFF (recul plus rapide que la
// recherche) — hsBackoffIntervalUs() de MinCalibration.cpp.
static uint32_t backoffIntervalUs(void) {
    if (g_intervalUs <= 300UL) return g_intervalUs;
    uint32_t v = g_intervalUs / 3UL;
    return (v < 100UL) ? 100UL : v;
}

// Message d'échec homing (équivalent des Serial.print AVR, via hwLog).
static void logHomingFail(uint8_t idx, const char* why) {
    (void)idx;
    hwLog(why);
    hwLog("[HOMING] No endstop found - homing aborted (check wiring or disable the axis in Motion Center)");
}

// ── FSM homing MIN d'un axe ───────────────────────────────────────────────
static void tickAxis(uint8_t idx) {
    ActuatorState  *s   = &ax[idx];
    const ActuatorConfig *cfg = &axCfg[idx];

    // Sens de recherche et de dégagement selon la position de l'endstop.
    // Direction EFFECTIVE : homing MAX exige un max calibré (HomingDir.h),
    // sinon repli sur un homing MIN classique.
    const bool   toMax      = HomingDir_EffectiveToMax(cfg->hometoMax, cfg->maxPos);
    const int8_t seekDir    = toMax ? (int8_t)+1 : (int8_t)-1; // vers endstop
    const int8_t releaseDir = (int8_t)(-seekDir);               // s'en éloigner

    switch (s->hs) {

        case HS_CHECK:
            s->moved = 0;
            if (readEndstop(idx)) {
                // Endstop déjà actif : partir du milieu pour avoir de la
                // marge de recul en BACKOFF.
                s->pos    = 32768U;
                s->target = 32768U;
                s->armed  = false;
                s->hs     = HS_BACKOFF;
            } else {
                // Endstop inactif : partir de l'extrémité opposée pour
                // couvrir toute la course pendant SEEK.
                const uint16_t startPos = toMax ? 0U : 65535U;
                s->pos    = startPos;
                s->target = startPos;
                s->armed  = true;
                s->hs     = HS_SEEK;
            }
            break;

        case HS_BACKOFF:
            if (!readEndstop(idx)) {
                // OFF stable atteint → armer la détection, lancer la recherche
                s->armed = true;
                s->moved = 0;
                s->hs    = HS_SEEK;
                break;
            }
            axStep(idx, releaseDir, backoffIntervalUs(), g_stepSize);
            break;

        case HS_SEEK: {
            if (s->armed && readEndstop(idx)) {
                // Endstop trouvé → phase de dégagement.
                // Recul = margin configurée, sinon valeur par défaut.
                s->backoffLeft = cfg->margin ? cfg->margin
                                             : HOMING_DEFAULT_RELEASE_BACKOFF;
                s->hs = HS_RELEASE;
                break;
            }

            // Échec 1 : trop de pas parcourus sans trouver l'endstop
            if (s->moved >= HOMING_MAX_SEEK_STEPS) {
                s->target = s->pos;       // stop immédiat
                s->homing = false;
                s->homed  = false;
                s->hs     = HS_ERROR;
                refreshConnectionLed();
                logHomingFail(idx, "[HOMING] aborted: max seek steps reached");
                break;
            }

            // Échec 2 : extrémité logique atteinte sans trigger endstop
            // (0 en homing MIN, 65535 en homing MAX)
            {
                const uint16_t failPos = toMax ? 65535U : 0U;
                if (s->pos == failPos) {
                    s->target = s->pos;   // stop immédiat
                    s->homing = false;
                    s->homed  = false;
                    s->hs     = HS_ERROR;
                    refreshConnectionLed();
                    logHomingFail(idx, "[HOMING] aborted: reached logical end without endstop trigger");
                    break;
                }
            }

            axStep(idx, seekDir, g_intervalUs, g_stepSize);
            break;
        }

        case HS_RELEASE:
            if (readEndstop(idx)) {
                // Encore sur la butée → continuer à s'en éloigner
                axStep(idx, releaseDir, g_intervalUs, g_stepSize);
                break;
            }

            if (s->backoffLeft > 0) {
                // Recul de sécurité après relâchement du contact
                if (axStep(idx, releaseDir, g_intervalUs, g_stepSize)) {
                    const uint16_t dec = (s->backoffLeft >= g_stepSize)
                                       ? g_stepSize : s->backoffLeft;
                    s->backoffLeft = (uint16_t)(s->backoffLeft - dec);
                }
                break;
            }

            // Attendre que le dernier pas planifié soit consommé
            if (s->target != s->pos) break;

            // Position de repos directionnelle (HomingDir.h) :
            //  - homing MIN : 0 (zero pose a margin de la butee) ;
            //  - homing MAX : max - 2*margin (haut de la plage mappee).
            {
                const uint16_t rest = HomingDir_RestPos(toMax, cfg->maxPos,
                                                        cfg->margin);
                s->pos    = rest;
                s->target = rest;
            }
            s->homed  = true;
            s->homing = false;
            s->hs     = HS_IDLE;
            hwLedSet("red");          // rouge = homing OK (convention AVR)
            break;

        case HS_ERROR:
            // Rester en erreur jusqu'à homingCancel() ou nouveau homingStart()
            break;

        default:
            break;
    }
}

// ── API publique homing ───────────────────────────────────────────────────

void homingStart(uint8_t idx_mask) {
    homingAbortClear();
    bool anyStarted = false;

    for (uint8_t i = 0; i < MAX_ACTUATORS; i++) {
        if (!(idx_mask & (uint8_t)(1u << i))) continue;
        if (!axCfg[i].connected)              continue;
        if (!axCfg[i].hasEndstop)             continue;
        if (axCfg[i].absoluteEncoder)         continue;
        // V1/V2 : seuls M5/M6 (idx 4-5) ont leurs butées lisibles par le
        // firmware. M1-M4 ont des butées mécaniques non câblées.
        if (boxCfg.version <= 2 && i < 4)     continue;
        if (ax[i].homed)                      continue;

        // Mi-course par défaut : BACKOFF a toujours de la marge si l'endstop
        // est déjà actif (HS_CHECK repositionnera pour le SEEK sinon).
        ax[i].pos         = 32768U;
        ax[i].target      = 32768U;
        ax[i].hs          = HS_CHECK;
        ax[i].homing      = true;
        ax[i].homed       = false;
        ax[i].moved       = 0;
        ax[i].backoffLeft = 0;
        ax[i].armed       = false;
        ax[i].lastStepUs  = 0;
        anyStarted = true;
    }

    if (anyStarted) hwLedSet("orange");
}

void homingCancel(void) {
    for (uint8_t i = 0; i < MAX_ACTUATORS; i++) {
        if (!ax[i].homing && ax[i].hs == HS_IDLE) continue;
        ax[i].target = ax[i].pos;     // stop immédiat
        ax[i].homing = false;
        ax[i].homed  = false;
        ax[i].hs     = HS_IDLE;
    }
    refreshConnectionLed();
}

void homingTick(void) {
    if (!hostConnected) { disableServo(); }
    if (g_homingAbort)  { homingCancel(); calibCancel(); homingAbortClear(); return; }

    for (uint8_t i = 0; i < MAX_ACTUATORS; i++) {
        if (ax[i].homing) tickAxis(i);
    }
}

bool homingIsActive(uint8_t i) { return i < MAX_ACTUATORS && ax[i].homing; }
bool homingIsDone  (uint8_t i) { return i < MAX_ACTUATORS && ax[i].homed;  }
bool homingIsError (uint8_t i) { return i < MAX_ACTUATORS && ax[i].hs == HS_ERROR; }

// ════════════════════════════════════════════════════════════════════════
//  Calibration complète — séquence au-dessus du homing MIN
//
//  CAL_MIN         : délègue à la FSM homing (zéro posé)
//  CAL_MAX_SEEK    : avance vers le MAX (sens opposé au homing), endstop armé
//                    après OFF stable, anti-timeout identique au SEEK MIN
//  CAL_MAX_RELEASE : dégagement + margin, puis maxPos = position courante
// ════════════════════════════════════════════════════════════════════════

static CalibState s_cal     [MAX_ACTUATORS] = {0};
static uint16_t   s_calMoved[MAX_ACTUATORS] = {0};   // anti-timeout SEEK MAX
static uint16_t   s_calBack [MAX_ACTUATORS] = {0};   // recul restant RELEASE
static bool       s_calArmed[MAX_ACTUATORS] = {0};

void calibStartDetectMax(uint8_t idx) {
    if (idx >= MAX_ACTUATORS)        return;
    if (!axCfg[idx].connected)       return;
    if (!axCfg[idx].hasEndstop)      return;
    if (!ax[idx].homed) {            // MAX se mesure depuis un zéro connu
        s_cal[idx] = CAL_ERROR;
        hwLog("[CALIB] DETECT_MAX requires homing first");
        return;
    }
    s_calMoved[idx] = 0;
    s_calBack [idx] = 0;
    s_calArmed[idx] = false;         // armé après OFF stable (on quitte la zone MIN)
    s_cal     [idx] = CAL_MAX_SEEK;
    hwLedSet("orange");
}

void calibStartFull(uint8_t idx) {
    if (idx >= MAX_ACTUATORS)   return;
    if (!axCfg[idx].connected)  return;
    if (!axCfg[idx].hasEndstop) return;
    ax[idx].homed      = false;                 // forcer un homing frais
    ax[idx].calibrated = false;
    homingStart((uint8_t)(1u << idx));
    s_cal[idx] = CAL_MIN;
}

void calibCancel(void) {
    for (uint8_t i = 0; i < MAX_ACTUATORS; i++) {
        if (s_cal[i] == CAL_MAX_SEEK || s_cal[i] == CAL_MAX_RELEASE) {
            ax[i].target = ax[i].pos;           // stop immédiat
        }
        if (s_cal[i] != CAL_IDLE) s_cal[i] = CAL_IDLE;
    }
}

static void calibTickAxis(uint8_t idx) {
    ActuatorState        *s   = &ax[idx];
    const ActuatorConfig *cfg = &axCfg[idx];

    // Sens "vers le MAX" = opposé au sens de homing
    const int8_t maxDir     = cfg->hometoMax ? (int8_t)-1 : (int8_t)+1;
    const int8_t releaseDir = (int8_t)(-maxDir);

    switch (s_cal[idx]) {

        case CAL_MIN:
            if (s->homing) break;               // homing encore en cours
            if (!s->homed || s->hs == HS_ERROR) {
                s_cal[idx] = CAL_ERROR;         // homing échoué
                break;
            }
            // Zéro posé → enchaîner sur la mesure du MAX
            s_calMoved[idx] = 0;
            s_calBack [idx] = 0;
            s_calArmed[idx] = false;
            s_cal     [idx] = CAL_MAX_SEEK;
            break;

        case CAL_MAX_SEEK: {
            // Armement : attendre une lecture OFF stable (on vient de la
            // zone MIN, le switch série peut encore être pressé).
            if (!s_calArmed[idx]) {
                if (!readEndstop(idx)) s_calArmed[idx] = true;
                else { axStep(idx, maxDir, g_intervalUs, g_stepSize); break; }
            }

            if (readEndstop(idx)) {
                // Endstop MAX trouvé → dégagement
                s_calBack[idx] = cfg->margin ? cfg->margin
                                             : HOMING_DEFAULT_RELEASE_BACKOFF;
                s_cal[idx] = CAL_MAX_RELEASE;
                break;
            }

            // Échec 1 : limite de pas atteinte sans endstop
            if (s_calMoved[idx] >= HOMING_MAX_SEEK_STEPS) {
                s->target  = s->pos;
                s_cal[idx] = CAL_ERROR;
                refreshConnectionLed();
                hwLog("[CALIB] DETECT_MAX aborted: max seek steps reached");
                break;
            }
            // Échec 2 : extrémité logique atteinte sans trigger
            if (s->pos == (cfg->hometoMax ? 0U : 65535U)) {
                s->target  = s->pos;
                s_cal[idx] = CAL_ERROR;
                refreshConnectionLed();
                hwLog("[CALIB] DETECT_MAX aborted: reached logical end without endstop");
                break;
            }

            if (axStep(idx, maxDir, g_intervalUs, g_stepSize)) {
                if (s_calMoved[idx] <= (uint16_t)(65535U - g_stepSize))
                    s_calMoved[idx] = (uint16_t)(s_calMoved[idx] + g_stepSize);
            }
            break;
        }

        case CAL_MAX_RELEASE:
            if (readEndstop(idx)) {
                axStep(idx, releaseDir, g_intervalUs, g_stepSize);
                break;
            }
            if (s_calBack[idx] > 0) {
                if (axStep(idx, releaseDir, g_intervalUs, g_stepSize)) {
                    const uint16_t dec = (s_calBack[idx] >= g_stepSize)
                                       ? g_stepSize : s_calBack[idx];
                    s_calBack[idx] = (uint16_t)(s_calBack[idx] - dec);
                }
                break;
            }
            if (s->target != s->pos) break;     // dernier pas à consommer

            // Mesure : la position de repos devient la course maxi utilisable
            // (convention AVR MOVE_TO_MAX : max = mPosition après dégagement)
            axCfg[idx].maxPos = s->pos;
            s->calibrated     = (s->pos != 0U);
            s->homed          = true;
            s_cal[idx]        = s->calibrated ? CAL_DONE : CAL_ERROR;
            hwLedSet("red");
            break;

        default:
            break;
    }
}

void calibTick(void) {
    for (uint8_t i = 0; i < MAX_ACTUATORS; i++) {
        if (s_cal[i] == CAL_MIN || s_cal[i] == CAL_MAX_SEEK
                                || s_cal[i] == CAL_MAX_RELEASE) {
            calibTickAxis(i);
        }
    }
}

bool calibIsBusy(uint8_t idx) {
    if (idx >= MAX_ACTUATORS) return false;
    return s_cal[idx] == CAL_MIN || s_cal[idx] == CAL_MAX_SEEK
                                 || s_cal[idx] == CAL_MAX_RELEASE;
}

CalibState calibGetState(uint8_t idx) {
    return (idx < MAX_ACTUATORS) ? s_cal[idx] : CAL_IDLE;
}

void calibAckResult(uint8_t idx) {
    if (idx >= MAX_ACTUATORS) return;
    if (s_cal[idx] == CAL_DONE || s_cal[idx] == CAL_ERROR) s_cal[idx] = CAL_IDLE;
}
