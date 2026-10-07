#pragma once
#include <stdint.h>
#include <stdbool.h>

// ============================================================================
//  PORTABILITÉ
//  Ce header est volontairement *sans* dépendance Arduino/HAL : il se compile
//  aussi bien côté ATmega32U4 (build Arduino/C++) que côté STM32 bare-metal,
//  y compris depuis une unité de compilation **C** (ex. main.c CubeMX).
//
//  - Les enums gardent un underlying type uint8_t en C++ (gain mémoire AVR),
//    et retombent sur un typedef enum standard en C (compatibilité CubeMX).
//  - Les symboles partagés (ax[], axCfg[], boxCfg, axStep, axMicros) sont
//    exposés en "C" pour pouvoir être référencés depuis main.c.
//  - La base de temps microseconde est fournie par la *plateforme* via
//    axMicros() (voir Actuator.cpp / main.c), pas par micros().
// ============================================================================

// MAX_ACTUATORS doit être défini avant l'inclusion (Globals.h, platformio.ini,
// ou symboles préprocesseur du projet CubeMX). Valeur de repli si absent.
#ifndef MAX_ACTUATORS
  #define MAX_ACTUATORS 7
#endif

// Endpark désactivé (SET ENDPARK 255) : les vérins restent en place au
// SH_DISABLE. Valeurs 0..100 = % de la course utile (max - 2*margin).
#ifndef ENDPARK_OFF
  #define ENDPARK_OFF 255u
#endif


// ================================================================
//  FSM de homing
// ================================================================
#ifdef __cplusplus
enum HomingState : uint8_t {
#else
typedef enum {
#endif
    HS_IDLE    = 0,  // pas de homing en cours
    HS_CHECK,        // lecture initiale endstop -> décide BACKOFF ou SEEK
    HS_BACKOFF,      // endstop actif au départ  -> recule vers MAX
    HS_SEEK,         // avance vers MIN, cherche l'endstop
    HS_RELEASE,      // endstop touché -> dégagement + zeroing
    HS_ERROR         // échec (timeout, endstop absent, pos==0 sans trigger)
#ifdef __cplusplus
};
#else
} HomingState;
#endif


// ================================================================
//  Caractéristiques d'un vérin  (persistées en EEPROM côté AVR)
// ================================================================
typedef struct ActuatorConfig {
    uint16_t maxPos;            // position max calibrée  (0 = non calibrée)
    uint16_t margin;            // recul de sécurité post-homing (steps)
    int8_t   dir;              // sens moteur (+1 / -1)
    bool     connected;         // présent physiquement
    bool     hasEndstop;        // butée MIN câblée (homing possible)
    bool     hastobehomed;      // homing demandé (pendant homingStart() -> homingTick())
    bool     absoluteEncoder;   // capteur absolu (pas de homing, ignore endstop)
    bool     hometoMax;         // homing vers max au lieu de min
    uint8_t  endParkPct;        // position de park fin de session, % de la course (255 = off)
} ActuatorConfig;


// ================================================================
//  État runtime d'un vérin
//  pos / target sont volatile : modifiés par l'ISR moteur (AVR) ou la
//  boucle de génération de pas (STM32).
// ================================================================
typedef struct ActuatorState {
    volatile uint16_t pos;       // position courante
    volatile uint16_t target;    // consigne courante
    bool          calibrated;    // max connu et valide
    bool          homed;         // zéro posé (position physique connue)
    bool          homing;        // homing en cours
    HomingState   hs;            // état FSM homing
    uint32_t      lastStepUs;    // µs du dernier pas planifié (cadence)
    uint16_t      moved;         // pas parcourus pendant SEEK (anti-timeout)
    uint16_t      backoffLeft;   // pas restants pendant la phase RELEASE
    bool          armed;         // détection endstop armée (OFF stable vu)
} ActuatorState;


// ================================================================
//  Processeur
// ================================================================
#ifdef __cplusplus
enum ProcessorType : uint8_t {
#else
typedef enum {
#endif
    PROC_ATMEGA32U4 = 0,
    PROC_STM32F1,                // STM32F103 (competition box 4 axes, USB CDC)
    PROC_STM32G4                 // STM32G431 (control box G4)
#ifdef __cplusplus
};
#else
} ProcessorType;
#endif


// ================================================================
//  Comportement du bouton d'arrêt d'urgence (E-Stop)
//  V1/V2/F103 : coupure hardware, firmware aveugle.
//  V3 (G4)     : état remonté au firmware, action configurable.
// ================================================================
#ifdef __cplusplus
enum EstopMode : uint8_t {
#else
typedef enum {
#endif
    ESTOP_HW_ONLY = 0,   // coupure hardware, firmware aveugle
    ESTOP_REPORTED       // firmware informé, action configurable
#ifdef __cplusplus
};
#else
} EstopMode;
#endif

#ifdef __cplusplus
enum EstopAction : uint8_t {
#else
typedef enum {
#endif
    ESTOP_ACTION_DISABLE_SERVOS = 0,  // coupe les servos
    ESTOP_ACTION_STOP_MOTION,         // arrête le mouvement, servos ON
    ESTOP_ACTION_NOTIFY_ONLY          // notifie le host seulement
#ifdef __cplusplus
};
#else
} EstopAction;
#endif


// ================================================================
//  Configuration de la control box
//  (compilée en dur selon BOX_VERSION, non persistée en EEPROM)
// ================================================================
typedef struct BoxConfig {
    uint8_t       version;        // 1, 2, 3, 4…
    ProcessorType processor;
    uint8_t       maxActuators;   // nombre max de vérins de ce hardware
    EstopMode     estopMode;
    EstopAction   estopAction;    // ignoré si estopMode == ESTOP_HW_ONLY
} BoxConfig;


// ----------------------------------------------------------------------------
//  Symboles partagés — définis dans Actuator.cpp, exposés en "C" pour main.c
// ----------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

extern ActuatorConfig axCfg[MAX_ACTUATORS];
extern ActuatorState  ax   [MAX_ACTUATORS];
extern BoxConfig      boxCfg;

// ================================================================
//  Base de temps microseconde — fournie PAR LA PLATEFORME.
//   - Arduino/AVR : wrapper sur micros()              (Actuator.cpp)
//   - STM32       : symbole faible par défaut (=0),    (Actuator.cpp)
//                   surchargé par une implé forte HAL  (main.c)
//  axStep() est le seul consommateur ; une box sans homing peut l'ignorer.
// ================================================================
uint32_t axMicros(void);

// ================================================================
//  Avance la consigne (target) d'un vérin d'un pas, en respectant un
//  intervalle de temps minimum.
//    idx        : index dans ax[] (0-based)
//    dir        : +1 = vers MAX, -1 = vers MIN
//    intervalUs : µs minimum entre deux pas (cadence)
//    stepSize   : amplitude du pas (steps)
//  Retourne true si un pas a été effectivement planifié.
// ================================================================
bool axStep(uint8_t idx, int8_t dir, uint32_t intervalUs, uint8_t stepSize);

#ifdef __cplusplus
}  // extern "C"
#endif
