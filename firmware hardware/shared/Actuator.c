// Actuator.c — coeur portable du modèle vérin (single-source ATmega32U4 + STM32)
//
// Compilable en C pur (arm-none-eabi-gcc, avr-gcc) ET linkable depuis du C++
// (le .ino AVR) grâce aux blocs extern "C" du header (actifs côté C++ seulement).
//
// BOX_VERSION et MAX_ACTUATORS proviennent des FLAGS DE BUILD :
//   - PlatformIO (AVR)   : build_flags = -DBOX_VERSION=2 -DMAX_ACTUATORS=7
//   - STM32CubeIDE (F103): Project > C/C++ Build > Settings > Preprocessor
//                          -> BOX_VERSION=4   (et éventuellement MAX_ACTUATORS=4)
// Ce fichier n'inclut JAMAIS Globals.h (qui est du C++) : il reste autonome.

#include "Actuator.h"

#if defined(ARDUINO)
  #include <Arduino.h>   // micros()
#endif

#ifndef BOX_VERSION
  #error "BOX_VERSION non défini. Ajoutez -DBOX_VERSION=<1|2|3|4> aux flags de build."
#endif


// ============================================================================
//  Base de temps microseconde (axMicros) — implémentation par défaut.
//   - Arduino : micros() natif.
//   - STM32   : symbole FAIBLE renvoyant 0 ; main.c fournit l'implé forte
//               (TIM2 étendu 32 bits). axStep() reste donc toujours linkable.
// ============================================================================
#if defined(ARDUINO)
uint32_t axMicros(void) { return micros(); }
#else
__attribute__((weak)) uint32_t axMicros(void) { return 0U; }
#endif


// Zéro-init complet. À peupler au démarrage (EEPROM côté AVR, en dur côté F103).
ActuatorConfig axCfg[MAX_ACTUATORS] = {0};
ActuatorState  ax   [MAX_ACTUATORS] = {0};


bool axStep(uint8_t idx, int8_t dir, uint32_t intervalUs, uint8_t stepSize) {
    if (idx >= MAX_ACTUATORS) return false;
    ActuatorState *s = &ax[idx];

    // Attendre que le pas précédent soit consommé par l'ISR / la boucle
    if (s->target != s->pos) return false;

    // Respecter la cadence (base de temps fournie par la plateforme)
    const uint32_t now = axMicros();
    if ((uint32_t)(now - s->lastStepUs) < intervalUs) return false;

    // Calculer la prochaine position (clamp 0…65535)
    int32_t next = (int32_t)s->pos + (int32_t)dir * (int32_t)stepSize;
    if (next < 0)      next = 0;
    if (next > 65535L) next = 65535L;

    s->target     = (uint16_t)next;
    s->lastStepUs = now;

    // Compter uniquement les pas vers le MIN (anti-timeout de SEEK)
    if (dir < 0 && s->moved <= (uint16_t)(65535U - stepSize))
        s->moved = (uint16_t)(s->moved + stepSize);

    return true;
}


// ============================================================================
//  Configuration hardware compilée selon BOX_VERSION
// ============================================================================
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
    .estopAction  = ESTOP_ACTION_DISABLE_SERVOS   // défaut, modifiable
};
#elif BOX_VERSION == 4
// Competition control box STM32F103 : 4 axes, USB CDC, relais + enable servo.
// Coupure E-Stop hardware -> ESTOP_HW_ONLY.
BoxConfig boxCfg = {
    .version      = 4,
    .processor    = PROC_STM32F1,
    .maxActuators = 4,
    .estopMode    = ESTOP_HW_ONLY,
    .estopAction  = ESTOP_ACTION_DISABLE_SERVOS   // ignoré (HW_ONLY)
};
#else
  #error "BOX_VERSION non supporté (valeurs valides : 1, 2, 3, 4)"
#endif
