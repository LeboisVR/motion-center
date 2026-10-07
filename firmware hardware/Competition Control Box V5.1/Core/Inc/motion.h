/**
 ******************************************************************************
 * @file    motion.h
 * @brief   Moteur de mouvement generique 7 axes (5 HW timers + 2 bit-bang).
 *
 *  API publique. La configuration materielle (handles timers, GPIO) est
 *  resolue dans motion.c a partir des symboles CubeMX (htimX, *_GPIO_Port,
 *  *_Pin). Voir motion_config.h pour les pre-requis CubeMX.
 *
 *  Boucle d'utilisation typique (main.c) :
 *      Motion_Init();
 *      while (1) {
 *          SimHub_Process();            // pousse les cibles via Motion_SetTarget
 *          ...
 *      }
 *
 *  Les callbacks HAL (HAL_TIM_OC_DelayElapsedCallback,
 *  HAL_TIM_PeriodElapsedCallback) sont implementes dans motion.c : ne pas les
 *  redefinir ailleurs.
 ******************************************************************************
 */
#ifndef MOTION_H
#define MOTION_H

#include "main.h"          /* stm32g4xx_hal.h + defines broches */
#include "motion_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise le moteur : compteurs (ext clock), generateurs (arretes),
 *        TIM6 (tick bit-bang). A appeler apres les MX_TIMx_Init() de CubeMX.
 */
void Motion_Init(void);

/**
 * @brief Fixe la cible d'un axe (pas signes). N'agit que si la cible change
 *        (declenche on_new_target pour les axes HW).
 * @param axis  index 0..MOTION_NUM_AXES-1
 * @param steps position cible en pas (signee)
 */
void Motion_SetTarget(uint8_t axis, int32_t steps);

/**
 * @brief Position courante d'un axe en pas (signee).
 *        Axes HW : derivee du compteur. Axes bit-bang : compteur logiciel.
 */
int32_t Motion_GetPosition(uint8_t axis);

/**
 * @brief Cible courante d'un axe (pas signes).
 */
int32_t Motion_GetTarget(uint8_t axis);

/**
 * @brief Arret immediat de tous les generateurs (HW) + gel des cibles
 *        bit-bang sur la position courante. Utilise par ESTOP / SH_DISABLE.
 */
void Motion_StopAll(void);

/**
 * @brief Redefinit la position courante comme zero logique de l'axe.
 *        (Utilise apres un homing : recale vzero/compteur sur la butee.)
 */
void Motion_ZeroHere(uint8_t axis);

/**
 * @brief Redefinit la position courante a une valeur arbitraire (pas signes).
 *        Utilise apres un homing sur la butee MAX (position = course).
 *        Axes bit-bang : exact. Axes HW timer : repli sur Motion_ZeroHere
 *        (le recalage compteur arbitraire n'est pas supporte).
 */
void Motion_SetHere(uint8_t axis, int32_t pos);

/**
 * @brief Plafonne la cadence de pas des axes bit-bang (pas/s).
 *        0 = pleine vitesse (~MOTION_BITBANG_TICK_HZ/2 par axe).
 *        Utilise pour ralentir le homing/calibration.
 */
void Motion_SetMaxSps(uint32_t sps);

/**
 * @brief Frequence STEP pleine vitesse des axes timer HW (pas/s) = vitesse de
 *        streaming SimHub. Bornee a [MOTION_STEP_HZ_MIN, MOTION_STEP_HZ_MAX].
 *        Le homing utilise une cadence plus basse via Motion_SetMaxSps().
 */
void Motion_SetStepHz(uint32_t hz);

#ifdef __cplusplus
}
#endif

#endif /* MOTION_H */
