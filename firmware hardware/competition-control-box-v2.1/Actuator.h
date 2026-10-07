#pragma once
#include <stdint.h>
#include <stdbool.h>

// MAX_ACTUATORS doit être défini avant l'inclusion (dans Globals.h ou platformio.ini)
#ifndef MAX_ACTUATORS
  #define MAX_ACTUATORS 7
#endif

// Endpark désactivé (SET ENDPARK 255) : les vérins restent en place au
// SH_DISABLE. Valeurs 0..100 = % de la course utile (max - 2*margin).
#ifndef ENDPARK_OFF
  #define ENDPARK_OFF 255u
#endif


// ================================================================
//  Caractéristiques d'un vérin  (persistées en EEPROM)
// ================================================================
struct ActuatorConfig {
    uint16_t maxPos;      // position max calibrée  (0 = non calibrée)
    uint16_t margin;      // recul de sécurité post-homing (steps)
    int8_t   dir;         // sens moteur (+1 / -1)
    bool     connected;   // présent physiquement
    bool     hasEndstop;  // butée MIN câblée (homing possible)
    bool     hastobehomed;      // homing demandé (pendant homingStart() → homingTick())
    bool     absoluteEncoder;    // capteur de position absolu (pas de homing nécessaire, ignore endstop)
    bool     hometoMax;          // homing vers max au lieu de min (si endstop à l'autre extrémité)
    uint8_t  endParkPct;         // position de park fin de session, % de la course (255 = off)
};

// ================================================================
//  FSM de homing
// ================================================================
enum HomingState : uint8_t {
    HS_IDLE    = 0,  // pas de homing en cours
    HS_CHECK,        // lecture initiale endstop → décide BACKOFF ou SEEK
    HS_BACKOFF,      // endstop actif au départ  → recule vers MAX
    HS_SEEK,         // avance vers MIN, cherche l'endstop
    HS_RELEASE,      // endstop touché → dégagement + zeroing
    HS_ERROR         // échec (timeout, endstop absent, pos==0 sans trigger)
};

// ================================================================
//  État runtime d'un vérin
//  pos / target sont volatile : modifiés par l'ISR moteur
// ================================================================
struct ActuatorState {
    volatile uint16_t pos;       // position courante  (écrite par ISR)
    volatile uint16_t target;    // consigne courante   (lue   par ISR)
    bool          calibrated;    // max connu et valide
    bool          homed;         // zéro posé (position physique connue)
    bool          homing;        // homing en cours
    HomingState   hs;            // état FSM homing
    uint32_t      lastStepUs;    // µs du dernier pas planifié (cadence)
    uint16_t      moved;         // pas parcourus pendant SEEK (anti-timeout)
    uint16_t      backoffLeft;   // pas restants pendant la phase RELEASE
    bool          armed;         // détection endstop armée (OFF stable vu)
};

// Tableaux globaux — définis dans Actuator.cpp
extern ActuatorConfig axCfg[MAX_ACTUATORS];
extern ActuatorState  ax   [MAX_ACTUATORS];

// ================================================================
//  Fonction de mouvement unique
//
//  Avance la consigne (target) d'un vérin d'un pas dans la direction
//  indiquée, en respectant un intervalle de temps minimum.
//
//  idx       : index dans ax[] (0-based)
//  dir       : +1 = vers MAX, -1 = vers MIN
//  intervalUs: µs minimum entre deux pas (cadence de homing)
//  stepSize  : amplitude du pas (en steps)
//
//  Retourne true si un pas a été effectivement planifié.
//  Ne fait rien et retourne false si :
//    - le pas précédent n'est pas encore consommé (target != pos)
//    - l'intervalle n'est pas encore écoulé
// ================================================================
bool axStep(uint8_t idx, int8_t dir, uint32_t intervalUs, uint8_t stepSize);

// ================================================================
//  Processeur
// ================================================================
enum ProcessorType : uint8_t {
    PROC_ATMEGA32U4 = 0,
    PROC_STM32G4
};

// ================================================================
//  Comportement du bouton d'arrêt d'urgence (E-Stop)
//
//  V1/V2 : le hardware coupe les servos directement, le firmware
//          ne sait pas que le bouton a été pressé.
//
//  V3    : l'état du bouton remonte au firmware via une entrée GPIO.
//          L'action à entreprendre est configurable par l'utilisateur.
// ================================================================
enum EstopMode : uint8_t {
    ESTOP_HW_ONLY = 0,   // V1/V2 : coupure hardware, firmware aveugle
    ESTOP_REPORTED       // V3    : firmware informé, action configurable
};

enum EstopAction : uint8_t {
    ESTOP_ACTION_DISABLE_SERVOS = 0,  // coupe les servos (même effet qu'HW_ONLY)
    ESTOP_ACTION_STOP_MOTION,         // arrête le mouvement, servos restent ON
    ESTOP_ACTION_NOTIFY_ONLY          // notifie le host, ne fait rien de plus
};

// ================================================================
//  Configuration de la control box
//  (compilée en dur selon BOX_VERSION, non persistée en EEPROM)
// ================================================================
struct BoxConfig {
    uint8_t       version;        // 1, 2, 3…
    ProcessorType processor;
    uint8_t       maxActuators;   // nombre max de vérins supportés par ce hardware
    EstopMode     estopMode;
    EstopAction   estopAction;    // ignoré si estopMode == ESTOP_HW_ONLY
};

// Instance globale — définie dans Actuator.cpp
extern BoxConfig boxCfg;
