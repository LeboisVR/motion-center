/**
 ******************************************************************************
 * @file    motion_stub.c
 * @brief   STUB TEMPORAIRE du moteur de mouvement (Option A).
 *
 *  Le vrai moteur (motion.c.bak) depend de 11 timers qui ne sont pas encore
 *  configures dans CubeMX. Ce stub fournit l'API publique de motion.h en
 *  no-op afin que le firmware compile et que Motion Center puisse se
 *  connecter (HELLO/GET/STATUS/ENABLE/DISABLE/!DFU) et faire la mise a jour
 *  DFU. Les actionneurs ne bougent pas : STEP memorise seulement la cible.
 *
 *  Pour retablir le vrai moteur : configurer les timers dans CubeMX (voir
 *  motion_config.h), renommer motion.c.bak -> motion.c et supprimer ce fichier.
 ******************************************************************************
 */
#include "motion.h"

#ifdef MOTION_USE_STUB

static int32_t s_target[MOTION_NUM_AXES];

void Motion_Init(void)
{
  for (uint8_t i = 0; i < MOTION_NUM_AXES; i++) { s_target[i] = 0; }
}

void Motion_SetTarget(uint8_t axis, int32_t steps)
{
  if (axis < MOTION_NUM_AXES) { s_target[axis] = steps; }
}

int32_t Motion_GetPosition(uint8_t axis)
{
  return (axis < MOTION_NUM_AXES) ? s_target[axis] : 0;
}

int32_t Motion_GetTarget(uint8_t axis)
{
  return (axis < MOTION_NUM_AXES) ? s_target[axis] : 0;
}

void Motion_StopAll(void)
{
  /* Rien a arreter en mode stub. */
}

void Motion_ZeroHere(uint8_t axis)
{
  if (axis < MOTION_NUM_AXES) { s_target[axis] = 0; }
}

#endif /* MOTION_USE_STUB */
