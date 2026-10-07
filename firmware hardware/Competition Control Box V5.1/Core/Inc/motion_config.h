/**
 ******************************************************************************
 * @file    motion_config.h
 * @brief   Configuration centrale du moteur de mouvement 7 axes (G474VET6).
 *
 *  Architecture (validee sur proto) :
 *    - 5 axes "high speed" (450-500 kHz) : 1 timer generateur PWM (STEP)
 *      + 1 timer compteur esclave chaine via ITR (External Clock Mode 1).
 *      Position = CNT - vzero. Output Compare (CCR1) du compteur declenche
 *      UNE interruption a la cible -> coupe le generateur (arret au pulse pres).
 *    - 2 axes "bit-bang" (50-100 kHz) : bascule GPIO depuis l'IT TIM6
 *      (200 kHz -> 2 IT = 1 pulse), comptage logiciel.
 *
 *  Mapping timers (couples generateur -> compteur) :
 *    S1 : TIM3_CH1  -> TIM2  (32 bits)
 *    S2 : TIM4_CH1  -> TIM5  (32 bits)
 *    S3 : TIM8_CH1  -> TIM1  (16 bits, extension overflow logicielle)
 *    S4 : TIM16_CH1 -> TIM20 (16 bits, extension overflow logicielle)
 *    S5 : TIM17_CH1 -> TIM15 (16 bits, extension overflow logicielle)
 *    S6, S7 : bit-bang GPIO + TIM6
 *
 *  NOTE BRING-UP : ce module ne configure PAS les timers. Il consomme les
 *  handles HAL generes par CubeMX (htim2/3/4/5/8/1/16/20/17/15/6).
 *  Cote CubeMX, prevoir :
 *    - Generateurs : PWM CH1 a la frequence STEP, duty 50%, demarres a l'arret.
 *    - Compteurs   : External Clock Mode 1 (SMS=111), TS = ITR du generateur
 *                    associe (a trouver au bring-up par balayage TS), CH1 en
 *                    Output Compare (no output), ARR = 0xFFFF (16b) / 0xFFFFFFFF
 *                    (32b). NVIC : IT Capture/Compare + IT Update (16b).
 *    - TIM16/17 n'ont pas de TRGO : leur OC1 sert de trigger ITR.
 *    - TIM6 : 200 kHz, IT Update activee (NVIC TIM6_DAC).
 ******************************************************************************
 */
#ifndef MOTION_CONFIG_H
#define MOTION_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------------- */
/*  Topologie des axes                                                       */
/* ------------------------------------------------------------------------- */
#define MOTION_NUM_AXES     7u                 /* S1..S7                      */
#define MOTION_NUM_HS_AXES  5u                 /* indices 0..4 (timers HW)    */
#define MOTION_NUM_BB_AXES  2u                 /* indices 5..6 (bit-bang)     */
#define MOTION_BB_FIRST     5u                 /* premier index bit-bang      */

/* Index symboliques */
enum {
  AX_S1 = 0, AX_S2, AX_S3, AX_S4, AX_S5,   /* high-speed (timers)            */
  AX_S6, AX_S7                              /* bit-bang                       */
};

/* ------------------------------------------------------------------------- */
/*  Reference de position des compteurs                                      */
/*    Composite 32 bits centre sur 0x80000000 -> position signee = comp-vzero*/
/*    16 bits : CNT demarre a 0x0000, mot haut logiciel a 0x8000.            */
/*    32 bits : CNT demarre a 0x80000000, mot haut a 0x0000.                 */
/* ------------------------------------------------------------------------- */
#define MOTION_VZERO        0x80000000u
#define MOTION_VZERO_HI16   0x8000u            /* mot haut initial (16 bits)  */

/* Temps de setup DIR avant le front STEP (~10 us @170 MHz, boucle NOP).     */
#define MOTION_DIR_SETUP_NOPS  1700u

/* Direction auto-probe moves each motor a few steps at boot; keep disabled by
  default to avoid any motion on reset. */
#define MOTION_ENABLE_DIR_PROBE 0u

/* Debug safety mode: force all axes (S1..S7) to software bitbang stepping.
  This bypasses hardware timer/counter chaining while bring-up is unstable.
  2026-09-23 : le mode timer (0) a ete re-tente et produit des mouvements
  saccades / depassements (dir-probe off + watchdog re-ancre la position).
  Rester en bit-bang tant que le bring-up timer n'est pas revalide au banc. */
#define MOTION_FORCE_BITBANG_ALL 1u

//1 bitbang, 0 timer

/* Horloge des timers generateurs STEP (S1..S5), en Hz. */
#define MOTION_STEP_GEN_CLK_HZ  170000000u
/* Frequence STEP max des axes timer (pas/s) = vitesse de streaming SimHub.
   Reglable via Motion Center (SET STEP_HZ) ; defaut = limite driver validee. */
#define MOTION_STEP_HZ_DEFAULT  200000u
#define MOTION_STEP_HZ_MIN      1000u
#define MOTION_STEP_HZ_MAX      450000u

/* Frequence du tick bit-bang TIM6 (2 ticks = 1 pulse). Derivee de la
  frequence STEP par axe : 200k pas/s (config validee banc) -> tick 400 kHz.
  Reglable a chaud via SET STEP_HZ (Motion_SetStepHz reprogramme TIM6). */
#define MOTION_BITBANG_TICK_HZ  (2u * MOTION_STEP_HZ_DEFAULT)

/* Bitbang pace divider on TIM6 tick: effective max pulse rate ~= TIM6 / (2*DIV).
  With TIM6 configured to ~140 kHz and DIV=1, max is about 70 kHz per axis. */
#if MOTION_FORCE_BITBANG_ALL
#define MOTION_BITBANG_TICK_DIV 1u
#else
#define MOTION_BITBANG_TICK_DIV 1u
#endif

/* ------------------------------------------------------------------------- */
/*  Protocole SimHub (USB CDC)                                               */
/*    Trame binaire : 'T' + MOTION_NUM_AXES x uint16 big-endian + '\n'.      */
/*    Pour 7 axes : 1 + 14 + 1 = 16 octets, terminateur 0x0A en index 15.    */
/*    (Le brief mentionnait "18 octets / position 13" : ces constantes sont  */
/*     derivees de MOTION_NUM_AXES pour rester coherentes — a confirmer au    */
/*     bring-up si la trame SimHub differe.)                                  */
/* ------------------------------------------------------------------------- */
#define SH_FRAME_TAG        'T'
#define SH_FRAME_TERM       0x0Au
#define SH_FRAME_LEN        (1u + 2u * MOTION_NUM_AXES + 1u)   /* = 16        */
#define SH_FRAME_TERM_IDX   (SH_FRAME_LEN - 1u)               /* = 15        */

/* Conversion valeur SimHub (uint16 0..65535) -> pas signes (centre = 0).
   Echelle 1:1 par defaut ; ajuster ici si AxisResolution/course differe.    */
static inline int32_t sh_value_to_steps(uint16_t v)
{
  return (int32_t)v - 32768;
}

#endif /* MOTION_CONFIG_H */
