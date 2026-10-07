#include "Actuator.h"
#include "Globals.h"   // BOX_VERSION, MAX_ACTUATORS
#include <Arduino.h>   // micros()

// Zéro-init complet.
// À peupler au démarrage depuis l'EEPROM dans FactoryReset() / loadConfig().
ActuatorConfig axCfg[MAX_ACTUATORS] = {};
ActuatorState  ax   [MAX_ACTUATORS] = {};

bool axStep(uint8_t idx, int8_t dir, uint32_t intervalUs, uint8_t stepSize) {
    if (idx >= MAX_ACTUATORS) return false;
    ActuatorState& s = ax[idx];

    // Attendre que le pas précédent soit consommé par l'ISR
    if (s.target != s.pos) return false;

    // Respecter la cadence
    const uint32_t now = micros();
    if ((uint32_t)(now - s.lastStepUs) < intervalUs) return false;

    // Calculer la prochaine position (clamp 0…65535)
    int32_t next = (int32_t)s.pos + (int32_t)dir * (int32_t)stepSize;
    if (next < 0)      next = 0;
    if (next > 65535L) next = 65535L;

    s.target     = (uint16_t)next;
    s.lastStepUs = now;

    // Compter uniquement les pas vers le MIN (pour l'anti-timeout de SEEK)
    if (dir < 0 && s.moved <= (uint16_t)(65535U - stepSize))
        s.moved = (uint16_t)(s.moved + stepSize);

    return true;
}

// Configuration hardware compilée selon BOX_VERSION
#if BOX_VERSION == 1
BoxConfig boxCfg = {
    .version      = 1,
    .processor    = PROC_ATMEGA32U4,
    .maxActuators = 5,
    .estopMode    = ESTOP_HW_ONLY,
    .estopAction  = ESTOP_ACTION_DISABLE_SERVOS   // ignoré (HW_ONLY)
};
#elif BOX_VERSION == 2
BoxConfig boxCfg = {
    .version      = 2,
    .processor    = PROC_ATMEGA32U4,
    .maxActuators = 6,
    .estopMode    = ESTOP_HW_ONLY,
    .estopAction  = ESTOP_ACTION_DISABLE_SERVOS   // ignoré (HW_ONLY)
};
#elif BOX_VERSION == 3
BoxConfig boxCfg = {
    .version      = 3,
    .processor    = PROC_STM32G4,
    .maxActuators = 7,
    .estopMode    = ESTOP_REPORTED,
    .estopAction  = ESTOP_ACTION_DISABLE_SERVOS   // valeur par défaut, modifiable par l'utilisateur
};
#else
  #error "BOX_VERSION non défini ou non supporté (valeurs valides : 1, 2, 3)"
#endif
