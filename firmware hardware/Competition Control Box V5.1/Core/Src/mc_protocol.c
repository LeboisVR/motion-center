/**
 ******************************************************************************
 * @file    mc_protocol.c
 * @brief   Compatibilite Motion Center : commandes texte ASCII + DFU.
 ******************************************************************************
 */
#include "mc_protocol.h"
#include "bl_update.h"
#include "cdc_tx.h"
#include "motion.h"
#include "motion_config.h"
#include "debug_uart.h"
#include "main.h"
#include "status_led.h"
#include "HomingDir.h"   /* direction de homing MIN/MAX (librairie commune) */
#include "PosMap.h"     /* mapping consigne SimHub -> course utile par moteur */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>

/* ------------------------------------------------------------------------- */
/*  Configuration produit                                                    */
/* ------------------------------------------------------------------------- */
/* L'app Motion Center exige REQUIRED_BOX_FIRMWARE_PREFIX = "2.2". */
#define MC_FW_VERSION     "2.2.6"
/* Numero de boitier rapporte dans STATUS (BOX=n). Version PRODUIT vue par
   l'utilisateur : V3 (le PCB V5.1 est une reference interne). */
#define MC_BOX_VERSION    3
/* Version mini de Motion Center requise (champ MCMIN). */
#define MC_MIN_VERSION    "1.1"
/* Vitesse de homing par defaut (pas/s) rapportee dans STATUS (HSPS).
   ~1300 sps = cadence constatee acceptable sur banc (Pn98=12, course 32767). */
#define MC_HOMING_SPS     1300u
/* Plage cible G474 (15 bits). */
#define MC_AXIS_MAX_STEPS 32767

/* Polarites (faciles a inverser selon le cablage des drivers). */
#define MC_ENABLE_ACTIVE_LOW   1   /* 1 : ENABLE driver actif a l'etat bas   */
#define MC_RELAIS_ACTIVE_HIGH  1   /* 1 : relais d'alim (PE7) ferme a l'etat HAUT */
#define MC_ESTOP_ACTIVE_LOW    0   /* 0 : ESTOP actif a l'etat haut           */

#define MC_ESTOP_BEHAV_DISABLE_SERVO 0u
#define MC_ESTOP_BEHAV_RETURN_PARK   1u
#define MC_ESTOP_BEHAV_STOP_HERE     2u
#define MC_ESTOP_BEHAV_MAX           MC_ESTOP_BEHAV_STOP_HERE
#define MC_PARK_REACHED_TOL_STEPS    4
/* Endpark desactive (SET ENDPARK 255) : les verins restent en place au
   SH_DISABLE. Valeurs 0..100 = % de la course utile (max - 2*margin). */
#define MC_ENDPARK_OFF               255u
#define MC_ENDSTOP_ACTIVE_LOW        1
#define MC_CAL_RELEASE_STEPS         1200
/* Marge de securite posee entre le point de liberation du switch et la
   reference (zero ou max mesure) : evite qu'un retour aux extremes
   (park 0, course pleine) ne refoule la butee. */
#define MC_CAL_SAFETY_MARGIN_STEPS   100
/* Distance max de recherche de butee. Si l'axe atteint cette cible sans
   declencher le switch, l'echec est remonte en SEEK_LIMIT (resolution trop
   fine : augmenter Pn98 sur le driver). */
#define MC_CAL_SEEK_NEG_STEPS        40000
#define MC_CAL_LOOP_DELAY_MS         2u
/* Timeout fixe conserve pour les degagements courts ; les recherches de
   butee utilisent cal_seek_timeout_ms() (dependant de la vitesse). */
#define MC_CAL_TIMEOUT_MS            50000u
/* Delai max pour que le driver monte son signal READY apres alimentation
   (relais) + ENABLE. Les drivers servo mettent typiquement 0.5 a 2 s. */
#define MC_READY_WAIT_MS             3000u

/* Magic d'entree DFU : DOIT correspondre a celui lu par le bootloader
   resident G4 (registre de sauvegarde TAMP->BKP0R). */
#define MC_DFU_MAGIC      0x00004F50UL

/* Reglages persistants : derniere page de la BANQUE 1. L'application tourne
   en banque 2, l'ecriture est donc read-while-write (pas de stall CPU/USB),
   et le bootloader resident (debut de banque 1) n'atteint jamais cette page.
   Les registres de sauvegarde TAMP ne convenaient pas : sans pile VBAT le
   domaine de backup est perdu des que la box est debranchee. */
#define MC_CFG_PAGE_ADDR   0x0803F800UL
#define MC_CFG_PAGE_INDEX  127u
#define MC_CFG_PAGE_SIZE   0x800UL
#define MC_CFG_SLOT_SIZE   128u
#define MC_CFG_SLOTS       (MC_CFG_PAGE_SIZE / MC_CFG_SLOT_SIZE)
#define MC_CFG_MAGIC       0x4753434DUL  /* 'MCSG' */
#define MC_CFG_VERSION     2u
/* Les reglages arrivent en rafale (Motion Center pousse toute la config a
   la connexion) : on attend le silence avant d'ecrire, pour une seule
   ecriture flash au lieu d'une par commande SET. */
#define MC_CFG_COMMIT_MS   500u

/* Vitesse de homing acceptee (bornes alignees sur Motion Center). */
#define MC_HOMING_SPS_MIN  50u
#define MC_HOMING_SPS_MAX  10000u

typedef struct {
  uint8_t connected;
  uint8_t calibrated;
  uint8_t homing_dir;
  uint8_t endpark;
  int32_t max;
  int32_t margin;
} mc_cfg_motor_t;                                 /* 12 octets */

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint8_t  estop_enabled;
  uint8_t  estop_behavior;
  uint16_t homing_sps;
  uint16_t reserved;
  mc_cfg_motor_t motor[MOTION_NUM_AXES];
  uint8_t  spare[MC_CFG_SLOT_SIZE - 16u - 12u * MOTION_NUM_AXES];
  uint32_t crc;
} mc_cfg_t;                                       /* = MC_CFG_SLOT_SIZE */

_Static_assert(sizeof(mc_cfg_t) == MC_CFG_SLOT_SIZE, "mc_cfg_t must fill one slot");

/* ------------------------------------------------------------------------- */
/*  Etat                                                                     */
/* ------------------------------------------------------------------------- */
static bool s_servo;
static bool s_estop_enabled;
static volatile bool s_estop_pending;
static uint8_t s_estop_led_phase;
static uint32_t s_estop_led_last_ms;
static uint8_t s_estop_line_prev_low;
static uint8_t s_estop_mode_prev;
static uint8_t s_estop_behavior;
static bool s_estop_return_park_pending;
static bool s_restart_required;

/* Etat par moteur, reglable via SET et rapporte dans STATUS. Sauvegarde en
   flash (mc_cfg_*) : ces reglages doivent survivre a un debranchement. */
typedef struct {
  bool    connected;
  bool    calibrated;
  bool    homed;        /* position connue dans la session (homing effectue) */
  int32_t max;
  int32_t margin;
  uint8_t homing_dir;   /* 0=MIN, 1=MAX (shared/HomingDir.h) */
  uint8_t endpark;      /* % de course rejoint au SH_DISABLE (255 = off) */
} motor_state_t;

static motor_state_t s_motor[MOTION_NUM_AXES];
static uint16_t      s_homing_sps;
static uint32_t      s_step_hz;      /* frequence STEP max axes timer (pas/s) */
static bool          s_cfg_dirty;
static uint32_t      s_cfg_dirty_ms;

/* Raison du dernier echec de calibration, remontee par USB dans
   "HOMING FAILED <raison>" pour diagnostiquer sans UART debug. */
static char s_cal_fail_reason[48];

typedef struct { GPIO_TypeDef *port; uint16_t pin; } io_t;

/* Pins ENABLE par moteur (M1..M7), index 0..6. */
static const io_t k_enable[MOTION_NUM_AXES] = {
  { ENABLEM1_GPIO_Port, ENABLEM1_Pin },
  { ENABLEM2_GPIO_Port, ENABLEM2_Pin },
  { ENABLEM3_GPIO_Port, ENABLEM3_Pin },
  { ENABLEM4_GPIO_Port, ENABLEM4_Pin },
  { ENABLEM5_GPIO_Port, ENABLEM5_Pin },
  { ENABLEM6_GPIO_Port, ENABLEM6_Pin },
  { ENABLEM7_GPIO_Port, ENABLEM7_Pin },
};

/* READYx inputs (presence/fault chain), active low on this hardware. */
static const io_t k_ready[MOTION_NUM_AXES] = {
  { READYM1_GPIO_Port, READYM1_Pin },
  { READYM2_GPIO_Port, READYM2_Pin },
  { READYM3_GPIO_Port, READYM3_Pin },
  { READYM4_GPIO_Port, READYM4_Pin },
  { READYM5_GPIO_Port, READYM5_Pin },
  { READYM6_GPIO_Port, READYM6_Pin },
  { READYM7_GPIO_Port, READYM7_Pin },
};

static const io_t k_endstop[MOTION_NUM_AXES] = {
  { ENDSTOPM1_GPIO_Port, ENDSTOPM1_Pin },
  { ENDSTOPM2_GPIO_Port, ENDSTOPM2_Pin },
  { ENDSTOPM3_GPIO_Port, ENDSTOPM3_Pin },
  { ENDSTOPM4_GPIO_Port, ENDSTOPM4_Pin },
  { ENDSTOPM5_GPIO_Port, ENDSTOPM5_Pin },
  { ENDSTOPM6_GPIO_Port, ENDSTOPM6_Pin },
  { ENDSTOPM7_GPIO_Port, ENDSTOPM7_Pin },
};

static void mc_apply_default_motor_config(void)
{
  /* User-visible V3 defaults:
     - M1..M4 connected
     - M5..M7 disconnected
     - MAX = 32767 on all axes */
  for (uint8_t i = 0; i < MOTION_NUM_AXES; i++) {
    s_motor[i].connected  = (i < 4u);
    s_motor[i].calibrated = (i < 4u);
    s_motor[i].homed      = false;
    s_motor[i].max        = MC_AXIS_MAX_STEPS;
    s_motor[i].margin     = 0;
    s_motor[i].homing_dir = HOMING_DIR_MIN;
    s_motor[i].endpark    = MC_ENDPARK_OFF;
  }
}

static void mc_apply_default_global_config(void)
{
  s_estop_enabled = false;
  s_estop_pending = false;
  s_estop_led_phase = 0u;
  s_estop_led_last_ms = 0u;
  s_estop_line_prev_low = 0u;
  s_estop_mode_prev = 0u;
  s_estop_behavior = MC_ESTOP_BEHAV_DISABLE_SERVO;
  s_estop_return_park_pending = false;
  s_restart_required = false;
}

static bool park_reached(void)
{
  for (uint8_t i = 0; i < MOTION_NUM_AXES; i++) {
    if (!s_motor[i].connected) {
      continue;
    }
    int32_t pos = Motion_GetPosition(i);
    if (pos > MC_PARK_REACHED_TOL_STEPS || pos < -MC_PARK_REACHED_TOL_STEPS) {
      return false;
    }
  }
  return true;
}

static bool endstop_is_active(uint8_t idx)
{
  uint8_t raw = (HAL_GPIO_ReadPin(k_endstop[idx].port, k_endstop[idx].pin) == GPIO_PIN_SET) ? 1u : 0u;
#if MC_ENDSTOP_ACTIVE_LOW
  return (raw == 0u);
#else
  return (raw != 0u);
#endif
}

static void axis_stop_here(uint8_t idx)
{
  int32_t pos = Motion_GetPosition(idx);
  Motion_SetTarget(idx, pos);
}

static bool wait_endstop_state(uint8_t idx, bool expectedActive, uint32_t timeoutMs)
{
  uint32_t t0 = HAL_GetTick();
  while ((uint32_t)(HAL_GetTick() - t0) < timeoutMs) {
    if ((bool)endstop_is_active(idx) == expectedActive) {
      return true;
    }
    if (MC_IsEstopMotionBlocked()) {
      return false;
    }
    HAL_Delay(MC_CAL_LOOP_DELAY_MS);
  }
  return ((bool)endstop_is_active(idx) == expectedActive);
}

/* Timeout de recherche de butee : duree du deplacement MC_CAL_SEEK_NEG_STEPS
   a la vitesse de homing courante + 5 s de marge. */
static uint32_t cal_seek_timeout_ms(void)
{
  uint32_t sps = (uint32_t)s_homing_sps;
  if (sps == 0u) { sps = 1u; }
  return (uint32_t)((uint64_t)MC_CAL_SEEK_NEG_STEPS * 1000u / sps) + 5000u;
}

/* Vrai si l'axe s'est arrete sur sa cible de recherche (limite de pas
   epuisee) au lieu de la butee. A appeler AVANT axis_stop_here(). */
static bool seek_limit_reached(uint8_t idx)
{
  int32_t d = Motion_GetPosition(idx) - Motion_GetTarget(idx);
  if (d < 0) { d = -d; }
  return d <= MC_PARK_REACHED_TOL_STEPS;
}

/* Attend que l'axe ait atteint sa cible (petits deplacements de calibration). */
static void wait_move_settle(uint8_t idx, uint32_t timeoutMs)
{
  uint32_t t0 = HAL_GetTick();
  while ((uint32_t)(HAL_GetTick() - t0) < timeoutMs) {
    int32_t d = Motion_GetPosition(idx) - Motion_GetTarget(idx);
    if (d < 0) { d = -d; }
    if (d <= 1) { return; }
    if (MC_IsEstopMotionBlocked()) { return; }
    HAL_Delay(MC_CAL_LOOP_DELAY_MS);
  }
}

static bool ready_is_present(uint8_t idx); /* forward declaration */
static uint8_t estop_raw_level(void);      /* forward declaration */
static void mc_cfg_touch(void);            /* forward declaration */

/* Attend que le driver signale READY (pin basse). Le READY ne monte que
   quelques centaines de ms apres alimentation/ENABLE : un test immediat
   echouerait systematiquement au premier homing apres SH_START. */
static bool wait_ready_present(uint8_t idx, uint32_t timeoutMs)
{
  uint32_t t0 = HAL_GetTick();
  while ((uint32_t)(HAL_GetTick() - t0) < timeoutMs) {
    if (ready_is_present(idx)) {
      return true;
    }
    if (MC_IsEstopMotionBlocked()) {
      return false;
    }
    HAL_Delay(MC_CAL_LOOP_DELAY_MS);
  }
  return ready_is_present(idx);
}

static bool calibrate_axis_min(uint8_t idx)
{
  uint32_t m = (uint32_t)(idx + 1u);

  if (!s_motor[idx].connected) {
    return true;
  }

  if (MC_IsEstopMotionBlocked()) {
    snprintf(s_cal_fail_reason, sizeof(s_cal_fail_reason),
             "M%lu ESTOP_LOCK raw=%u", (unsigned long)m, (unsigned)estop_raw_level());
    DebugUart_Printf("[CAL] M%lu FAILED: ESTOP/restart lock\r\n", (unsigned long)m);
    return false;
  }

  if (!wait_ready_present(idx, MC_READY_WAIT_MS)) {
    snprintf(s_cal_fail_reason, sizeof(s_cal_fail_reason),
             "M%lu NO_READY", (unsigned long)m);
    DebugUart_Printf("[CAL] M%lu FAILED: READY not present\r\n", (unsigned long)m);
    return false;
  }

  DebugUart_Printf("[CAL] M%lu start MIN homing\r\n", (unsigned long)m);
  s_motor[idx].calibrated = false;

  /* Direction effective (shared/HomingDir.h) : homing MAX exige un max
     calibre, sinon repli sur un homing MIN classique. */
  const bool toMax = HomingDir_EffectiveToMax(
      s_motor[idx].homing_dir == HOMING_DIR_MAX,
      (s_motor[idx].max > 0) ? (uint32_t)s_motor[idx].max : 0u);
  const int32_t releaseSign = toMax ? -1 : +1;   /* s'eloigner de la butee  */
  const int32_t seekSign    = toMax ? +1 : -1;   /* aller vers la butee     */

  if (endstop_is_active(idx)) {
    int32_t p = Motion_GetPosition(idx);
    Motion_SetTarget(idx, p + releaseSign * MC_CAL_RELEASE_STEPS);
    if (!wait_endstop_state(idx, false, 3000u)) {
      axis_stop_here(idx);
      snprintf(s_cal_fail_reason, sizeof(s_cal_fail_reason),
               "M%lu ENDSTOP_STUCK", (unsigned long)m);
      DebugUart_Printf("[CAL] M%lu FAILED: endstop stuck active\r\n", (unsigned long)m);
      return false;
    }
    axis_stop_here(idx);
    HAL_Delay(20u);
  }

  {
    int32_t start = Motion_GetPosition(idx);
    Motion_SetTarget(idx, start + seekSign * MC_CAL_SEEK_NEG_STEPS);
  }

  if (!wait_endstop_state(idx, true, cal_seek_timeout_ms())) {
    bool limit = seek_limit_reached(idx);
    axis_stop_here(idx);
    snprintf(s_cal_fail_reason, sizeof(s_cal_fail_reason),
             limit ? "M%lu SEEK_LIMIT" : "M%lu MIN_TIMEOUT", (unsigned long)m);
    DebugUart_Printf("[CAL] M%lu FAILED: %s\r\n", (unsigned long)m,
                     limit ? "seek step limit" : "MIN endstop timeout");
    return false;
  }

  axis_stop_here(idx);
  HAL_Delay(20u);

  /* Degagement de securite AVANT de poser la reference : liberer le switch
     puis s'en ecarter encore de MC_CAL_SAFETY_MARGIN_STEPS. Sans cela le
     zero (ou le max) reste SUR le switch presse et tout retour a l'extreme
     refoule la butee (force -> alarme driver). */
  {
    int32_t p = Motion_GetPosition(idx);
    Motion_SetTarget(idx, p + releaseSign * MC_CAL_RELEASE_STEPS);
    if (!wait_endstop_state(idx, false, 3000u)) {
      axis_stop_here(idx);
      snprintf(s_cal_fail_reason, sizeof(s_cal_fail_reason),
               "M%lu ENDSTOP_STUCK", (unsigned long)m);
      DebugUart_Printf("[CAL] M%lu FAILED: endstop stuck after homing\r\n", (unsigned long)m);
      return false;
    }
    axis_stop_here(idx);
    HAL_Delay(20u);
    p = Motion_GetPosition(idx);
    Motion_SetTarget(idx, p + releaseSign * MC_CAL_SAFETY_MARGIN_STEPS);
    wait_move_settle(idx, 3000u);
    axis_stop_here(idx);
    HAL_Delay(20u);
  }

  if (toMax) {
    /* Butee MAX : la position posee est la course calibree (margin geree
       cote mapping sur cette box). */
    Motion_SetHere(idx, s_motor[idx].max);
    Motion_SetTarget(idx, s_motor[idx].max);
  } else {
    Motion_ZeroHere(idx);
    Motion_SetTarget(idx, 0);
  }
  s_motor[idx].calibrated = true;
  s_motor[idx].homed = true;
  if (s_motor[idx].max <= 0) {
    s_motor[idx].max = MC_AXIS_MAX_STEPS;
  }
  mc_cfg_touch();

  DebugUart_Printf("[CAL] M%lu done\r\n", (unsigned long)m);
  return true;
}

/* Mesure de la course MAX (apres un homing MIN reussi, position = 0).
   Convention cablage AVR : les switches MIN et MAX sont en serie sur la
   MEME entree endstop. On avance vers le MAX jusqu'au declenchement, on
   degage jusqu'a liberation, et la position degagee devient le max.
   returnToZero : retour au repos (0) en fin de mesure (calibration complete) ;
   false = rester au max (DO MOVE_TO_MAX). */
static bool calibrate_axis_max(uint8_t idx, bool returnToZero)
{
  uint32_t m = (uint32_t)(idx + 1u);

  DebugUart_Printf("[CAL] M%lu start MAX measure\r\n", (unsigned long)m);

  /* Le homing MIN laisse l'axe SUR le switch MIN (le zero est pose au point
     de declenchement, sans degagement). L'entree endstop est donc encore
     active ici : il faut d'abord degager vers le MAX jusqu'a liberation,
     sinon la recherche "trouve" immediatement le switch MIN et le
     degagement partirait du mauvais cote (l'axe force sur le min). */
  if (endstop_is_active(idx)) {
    int32_t p = Motion_GetPosition(idx);
    Motion_SetTarget(idx, p + MC_CAL_RELEASE_STEPS);
    if (!wait_endstop_state(idx, false, 3000u)) {
      axis_stop_here(idx);
      snprintf(s_cal_fail_reason, sizeof(s_cal_fail_reason),
               "M%lu ENDSTOP_STUCK", (unsigned long)m);
      DebugUart_Printf("[CAL] M%lu FAILED: MIN endstop stuck before MAX seek\r\n",
                       (unsigned long)m);
      return false;
    }
    axis_stop_here(idx);
    HAL_Delay(20u);
  }

  /* Recherche de la butee MAX. */
  {
    int32_t start = Motion_GetPosition(idx);
    Motion_SetTarget(idx, start + MC_CAL_SEEK_NEG_STEPS);
  }
  if (!wait_endstop_state(idx, true, cal_seek_timeout_ms())) {
    bool limit = seek_limit_reached(idx);
    axis_stop_here(idx);
    snprintf(s_cal_fail_reason, sizeof(s_cal_fail_reason),
             limit ? "M%lu SEEK_LIMIT" : "M%lu MAX_TIMEOUT", (unsigned long)m);
    DebugUart_Printf("[CAL] M%lu FAILED: %s\r\n", (unsigned long)m,
                     limit ? "seek step limit" : "MAX endstop timeout");
    return false;
  }
  axis_stop_here(idx);
  HAL_Delay(20u);

  /* Degagement : reculer jusqu'a liberation du switch, puis marge de
     securite supplementaire — le max mesure reste a distance du switch. */
  {
    int32_t p = Motion_GetPosition(idx);
    Motion_SetTarget(idx, p - MC_CAL_RELEASE_STEPS);
  }
  if (!wait_endstop_state(idx, false, 3000u)) {
    axis_stop_here(idx);
    snprintf(s_cal_fail_reason, sizeof(s_cal_fail_reason),
             "M%lu ENDSTOP_STUCK", (unsigned long)m);
    DebugUart_Printf("[CAL] M%lu FAILED: MAX endstop stuck\r\n", (unsigned long)m);
    return false;
  }
  axis_stop_here(idx);
  HAL_Delay(20u);
  {
    int32_t p = Motion_GetPosition(idx);
    Motion_SetTarget(idx, p - MC_CAL_SAFETY_MARGIN_STEPS);
    wait_move_settle(idx, 3000u);
    axis_stop_here(idx);
    HAL_Delay(20u);
  }

  int32_t maxPos = Motion_GetPosition(idx);
  if (maxPos < 100) {
    snprintf(s_cal_fail_reason, sizeof(s_cal_fail_reason),
             "M%lu MAX_TOO_SMALL", (unsigned long)m);
    DebugUart_Printf("[CAL] M%lu FAILED: measured max too small\r\n", (unsigned long)m);
    return false;
  }
  /* Remonter la course mesuree a Motion Center (CLAMPED si > plage 15 bits). */
  {
    char msg[72];
    snprintf(msg, sizeof(msg), "STROKE M%lu %ld%s\n", (unsigned long)m,
             (long)maxPos, (maxPos > MC_AXIS_MAX_STEPS) ? " CLAMPED" : "");
    CDC_SendStr(msg);
  }
  if (maxPos > MC_AXIS_MAX_STEPS) {
    maxPos = MC_AXIS_MAX_STEPS;
  }
  s_motor[idx].max = maxPos;
  s_motor[idx].calibrated = true;
  mc_cfg_touch();
  if (returnToZero) {
    /* Retour au repos (0) a vitesse reduite avant de rendre la main. */
    Motion_SetTarget(idx, 0);
    uint32_t timeoutMs = (uint32_t)maxPos * 1000u /
                         (s_homing_sps ? s_homing_sps : 1u) + 5000u;
    uint32_t t0 = HAL_GetTick();
    for (;;) {
      int32_t p = Motion_GetPosition(idx);
      if (p <= MC_PARK_REACHED_TOL_STEPS && p >= -MC_PARK_REACHED_TOL_STEPS) { break; }
      if ((uint32_t)(HAL_GetTick() - t0) > timeoutMs) { break; }
      if (MC_IsEstopMotionBlocked()) { break; }
      HAL_Delay(MC_CAL_LOOP_DELAY_MS);
    }
  }

  DebugUart_Printf("[CAL] M%lu MAX = %ld\r\n", (unsigned long)m, (long)maxPos);
  return true;
}

static uint32_t mc_cfg_crc(const mc_cfg_t *c)
{
  const uint8_t *p = (const uint8_t *)c;
  uint32_t h = 2166136261UL;                      /* FNV-1a 32 bits */
  for (size_t i = 0; i < offsetof(mc_cfg_t, crc); i++) {
    h = (uint32_t)((h ^ p[i]) * 16777619UL);
  }
  return h;
}

/* Parcourt le journal : retourne le dernier enregistrement valide et,
   dans *outFreeSlot, le premier emplacement encore vierge. */
static bool mc_cfg_scan(mc_cfg_t *out, uint32_t *outFreeSlot)
{
  bool found = false;
  uint32_t slot = 0u;

  for (; slot < MC_CFG_SLOTS; slot++) {
    const mc_cfg_t *rec =
        (const mc_cfg_t *)(MC_CFG_PAGE_ADDR + slot * MC_CFG_SLOT_SIZE);
    if (rec->magic == 0xFFFFFFFFUL) {
      break;                                      /* fin du journal */
    }
    if (rec->magic == MC_CFG_MAGIC && rec->version == MC_CFG_VERSION &&
        rec->crc == mc_cfg_crc(rec)) {
      *out  = *rec;
      found = true;
    }
  }

  *outFreeSlot = slot;
  return found;
}

/* Sauvegarde tous les reglages (globaux + par moteur) en flash. Ecriture
   par ajout dans le journal : la page n'est effacee que lorsque tous les
   emplacements sont pleins, et rien n'est ecrit si la config est inchangee
   (usure flash minimale). */
static void mc_cfg_save(void)
{
  mc_cfg_t cur;
  uint32_t slot = 0u;
  bool has = mc_cfg_scan(&cur, &slot);

  mc_cfg_t nw;
  memset(&nw, 0, sizeof(nw));
  nw.magic          = MC_CFG_MAGIC;
  nw.version        = MC_CFG_VERSION;
  nw.estop_enabled  = s_estop_enabled ? 1u : 0u;
  nw.estop_behavior = s_estop_behavior;
  nw.homing_sps     = s_homing_sps;
  for (uint8_t i = 0; i < MOTION_NUM_AXES; i++) {
    nw.motor[i].connected  = s_motor[i].connected ? 1u : 0u;
    nw.motor[i].calibrated = s_motor[i].calibrated ? 1u : 0u;
    nw.motor[i].homing_dir = s_motor[i].homing_dir;
    nw.motor[i].endpark    = s_motor[i].endpark;
    nw.motor[i].max        = s_motor[i].max;
    nw.motor[i].margin     = s_motor[i].margin;
  }
  nw.crc            = mc_cfg_crc(&nw);

  if (has && memcmp(&cur, &nw, sizeof(nw)) == 0) {
    return;
  }

  HAL_FLASH_Unlock();
  __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);

  if (slot >= MC_CFG_SLOTS) {
    FLASH_EraseInitTypeDef e;
    memset(&e, 0, sizeof(e));
    e.TypeErase = FLASH_TYPEERASE_PAGES;
    e.Banks     = FLASH_BANK_1;
    e.Page      = MC_CFG_PAGE_INDEX;
    e.NbPages   = 1u;
    uint32_t err = 0u;
    if (HAL_FLASHEx_Erase(&e, &err) != HAL_OK) {
      HAL_FLASH_Lock();
      DebugUart_Print("[MC] CFG erase failed\r\n");
      return;
    }
    slot = 0u;
  }

  uint64_t dw[MC_CFG_SLOT_SIZE / 8u];
  memcpy(dw, &nw, sizeof(dw));
  const uint32_t addr = MC_CFG_PAGE_ADDR + slot * MC_CFG_SLOT_SIZE;
  for (uint32_t i = 0u; i < (MC_CFG_SLOT_SIZE / 8u); i++) {
    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                          addr + 8u * i, dw[i]) != HAL_OK) {
      DebugUart_Print("[MC] CFG program failed\r\n");
      break;
    }
  }
  HAL_FLASH_Lock();
  DebugUart_Printf("[MC] CFG saved slot=%lu estop=%u beh=%u hsps=%u\r\n",
                   (unsigned long)slot, (unsigned)nw.estop_enabled,
                   (unsigned)nw.estop_behavior, (unsigned)nw.homing_sps);
}

static void mc_cfg_touch(void)
{
  s_cfg_dirty    = true;
  s_cfg_dirty_ms = HAL_GetTick();
}

static void mc_cfg_flush(void)
{
  if (s_cfg_dirty) {
    s_cfg_dirty = false;
    mc_cfg_save();
  }
}

static uint8_t estop_raw_level(void)
{
  return (HAL_GPIO_ReadPin(ESTOP_GPIO_Port, ESTOP_Pin) == GPIO_PIN_SET) ? 1u : 0u;
}

static uint8_t estop_is_active(void)
{
#if MC_ESTOP_ACTIVE_LOW
  return (uint8_t)(!estop_raw_level());
#else
  return estop_raw_level();
#endif
}

/* ------------------------------------------------------------------------- */
/*  Helpers                                                                  */
/* ------------------------------------------------------------------------- */
static void enable_write(const io_t *io, bool on)
{
#if MC_ENABLE_ACTIVE_LOW
  HAL_GPIO_WritePin(io->port, io->pin, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
#else
  HAL_GPIO_WritePin(io->port, io->pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
#endif
}

static bool ready_is_present(uint8_t idx)
{
  return HAL_GPIO_ReadPin(k_ready[idx].port, k_ready[idx].pin) == GPIO_PIN_RESET;
}

static void relais_write(bool on)
{
#if MC_RELAIS_ACTIVE_HIGH
  HAL_GPIO_WritePin(RELAIS_GPIO_Port, RELAIS_Pin, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
#else
  HAL_GPIO_WritePin(RELAIS_GPIO_Port, RELAIS_Pin, on ? GPIO_PIN_RESET : GPIO_PIN_SET);
#endif
}

static void servo_set(bool on)
{
  if (on) {
    DebugUart_Print("[MC] SERVO ON\r\n");
    for (uint8_t i = 0; i < MOTION_NUM_AXES; i++) {
      /* ENABLE inconditionnel sur les axes connectes : READY ne monte
         qu'APRES alimentation (relais) + ENABLE, donc le conditionner a
         READY creait un blocage oeuf/poule au demarrage. */
      enable_write(&k_enable[i], s_motor[i].connected);
    }
    relais_write(true);
    s_servo = true;
  } else {
    DebugUart_Print("[MC] SERVO OFF\r\n");
    Motion_StopAll();
    relais_write(false);
    for (uint8_t i = 0; i < MOTION_NUM_AXES; i++) {
      enable_write(&k_enable[i], false);
      s_motor[i].homed = false;   /* drivers coupes : position perdue */
    }
    s_servo = false;
  }
}

static void estop_apply_behavior(void)
{
  switch (s_estop_behavior) {
    case MC_ESTOP_BEHAV_RETURN_PARK:
      DebugUart_Print("[MC] ESTOP action: RETURN_TO_PARK\r\n");
      s_estop_return_park_pending = false;
      MC_EnableConnectedMotors();
      for (uint8_t i = 0; i < MOTION_NUM_AXES; i++) {
        if (s_motor[i].connected) {
          Motion_SetTarget(i, 0);
        }
      }
      s_estop_return_park_pending = true;
      break;

    case MC_ESTOP_BEHAV_STOP_HERE:
      DebugUart_Print("[MC] ESTOP action: STOP_HERE\r\n");
      s_estop_return_park_pending = false;
      Motion_StopAll();
      break;

    case MC_ESTOP_BEHAV_DISABLE_SERVO:
    default:
      DebugUart_Print("[MC] ESTOP action: DISABLE_SERVO\r\n");
      s_estop_return_park_pending = false;
      servo_set(false);
      break;
  }
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
  if (GPIO_Pin == ESTOP_Pin && s_estop_enabled) {
    s_estop_pending = true;
  }
}

void MC_Process(void)
{
  if (s_cfg_dirty &&
      (uint32_t)(HAL_GetTick() - s_cfg_dirty_ms) >= MC_CFG_COMMIT_MS) {
    mc_cfg_flush();
  }

  {
    uint8_t estop_raw = estop_raw_level();
    uint8_t estop_active = estop_is_active();
    uint8_t estop_active_prev = s_estop_line_prev_low;
    uint8_t estop_mode = s_estop_enabled ? 1u : 0u;

    if (estop_mode != s_estop_mode_prev || estop_active != s_estop_line_prev_low) {
      DebugUart_Printf("[MC] ESTOP mode=%s pin_raw=%u pin_active=%u\r\n",
                       estop_mode ? "ENABLED" : "DISABLED",
                       (unsigned)estop_raw,
                       (unsigned)estop_active);
      s_estop_mode_prev = estop_mode;
      s_estop_line_prev_low = estop_active;
    }

    /* Polling fallback: trigger on active edge even if EXTI edge is missed. */
    if (s_estop_enabled && estop_active && !estop_active_prev) {
      s_estop_pending = true;
    }

    if (s_estop_enabled && estop_active) {
      uint32_t now = HAL_GetTick();
      if ((uint32_t)(now - s_estop_led_last_ms) >= 150u) {
        s_estop_led_last_ms = now;
        s_estop_led_phase ^= 1u;
        if (s_estop_led_phase) {
          StatusLed_ShowRgb(255u, 96u, 0u);
        } else {
          StatusLed_ShowRgb(0u, 0u, 0u);
        }
      }
    }

    if (s_estop_return_park_pending && park_reached()) {
      s_estop_return_park_pending = false;
      servo_set(false);
      s_restart_required = true;
      DebugUart_Print("[MC] ESTOP return park complete: SERVO OFF, reboot required\r\n");
    }
  }

  if (!s_estop_pending) {
    return;
  }
  s_estop_pending = false;
  DebugUart_Print("[MC] ESTOP triggered\r\n");
  estop_apply_behavior();
  CDC_SendStr("ESTOP\n");
}

/* Construit et envoie la ligne STATUS attendue par parse_status_line(). */
static void send_status(void)
{
  char buf[288];
  int n = snprintf(buf, sizeof(buf),
                   "STATUS FW=%s BOX=%d SERVO=%d HSPS=%u STPHZ=%lu ESTOP=%d ESTOPB=%u MCMIN=%s",
                   MC_FW_VERSION, MC_BOX_VERSION, s_servo ? 1 : 0,
                   (unsigned)s_homing_sps, (unsigned long)s_step_hz,
                   s_estop_enabled ? 1 : 0,
                   (unsigned)s_estop_behavior,
                   MC_MIN_VERSION);

  /* Champs par moteur : Mx=connecte,calibre,max,marge,hdir,endpark (RAM). */
  for (uint8_t i = 0; i < MOTION_NUM_AXES; i++) {
    if (n < 0 || n >= (int)sizeof(buf)) { break; }
    n += snprintf(buf + n, sizeof(buf) - (size_t)n,
                  " M%u=%u,%u,%ld,%ld,%u,%u", (unsigned)(i + 1u),
                  s_motor[i].connected ? 1u : 0u,
                  s_motor[i].calibrated ? 1u : 0u,
                  (long)s_motor[i].max, (long)s_motor[i].margin,
                  (unsigned)s_motor[i].homing_dir,
                  (unsigned)s_motor[i].endpark);
  }
  if (n >= 0 && n < (int)sizeof(buf) - 1) {
    buf[n++] = '\n';
    buf[n]   = '\0';
  } else {
    buf[sizeof(buf) - 2] = '\n';
    buf[sizeof(buf) - 1] = '\0';
  }
  CDC_SendStr(buf);
}

/* Reboot vers le bootloader resident (pose un magic puis reset).
   Sur STM32G4 il n'y a pas de BKP->DR1 (F1) : on utilise un registre de
   sauvegarde TAMP du domaine de backup, relu par le bootloader. */
static void enter_dfu(void)
{
  mc_cfg_flush();                 /* ne pas perdre un reglage juste modifie */
  CDC_SendStr("DFU\n");
  /* Laisser le CDC transmettre la reponse avant le reset. */
  for (volatile uint32_t i = 0; i < 4000000UL; i++) { __NOP(); }

  __HAL_RCC_PWR_CLK_ENABLE();
  HAL_PWR_EnableBkUpAccess();
#ifdef __HAL_RCC_RTCAPB_CLK_ENABLE
  __HAL_RCC_RTCAPB_CLK_ENABLE();
#endif
  TAMP->BKP0R = MC_DFU_MAGIC;
  __DSB();
  NVIC_SystemReset();
}

/* "CMD" en tete de ligne (commande seule ou suivie d'un espace) ? */
static bool is_cmd(const char *line, const char *cmd)
{
  size_t l = strlen(cmd);
  if (strncmp(line, cmd, l) != 0) { return false; }
  return (line[l] == '\0') || (line[l] == ' ');
}

/* ------------------------------------------------------------------------- */
/*  API                                                                      */
/* ------------------------------------------------------------------------- */
void MC_Init(void)
{
  mc_cfg_t persisted;
  uint32_t freeSlot = 0u;
  bool has_persisted = mc_cfg_scan(&persisted, &freeSlot);

  s_servo = false;
  s_homing_sps = MC_HOMING_SPS;
  s_step_hz = MOTION_STEP_HZ_DEFAULT;
  mc_apply_default_motor_config();
  mc_apply_default_global_config();
  s_cfg_dirty = false;
  if (has_persisted) {
    s_estop_enabled  = (persisted.estop_enabled != 0u);
    s_estop_behavior = (persisted.estop_behavior > MC_ESTOP_BEHAV_MAX)
                         ? MC_ESTOP_BEHAV_DISABLE_SERVO
                         : persisted.estop_behavior;
    if (persisted.homing_sps >= MC_HOMING_SPS_MIN &&
        persisted.homing_sps <= MC_HOMING_SPS_MAX) {
      s_homing_sps = persisted.homing_sps;
    }
    for (uint8_t i = 0; i < MOTION_NUM_AXES; i++) {
      int32_t max = persisted.motor[i].max;
      if (max < 0) { max = 0; }
      if (max > MC_AXIS_MAX_STEPS) { max = MC_AXIS_MAX_STEPS; }
      s_motor[i].connected  = (persisted.motor[i].connected != 0u);
      s_motor[i].calibrated = (persisted.motor[i].calibrated != 0u);
      s_motor[i].homing_dir = (persisted.motor[i].homing_dir != 0u)
                                ? HOMING_DIR_MAX : HOMING_DIR_MIN;
      s_motor[i].endpark    = (persisted.motor[i].endpark <= 100u)
                                ? persisted.motor[i].endpark : MC_ENDPARK_OFF;
      s_motor[i].max        = max;
      s_motor[i].margin     = persisted.motor[i].margin;
    }
    s_estop_mode_prev = s_estop_enabled ? 0u : 1u; /* force first-state log */
    DebugUart_Printf("[MC] CFG restored: estop=%s beh=%u hsps=%u\r\n",
                     s_estop_enabled ? "ENABLED" : "DISABLED",
                     (unsigned)s_estop_behavior,
                     (unsigned)s_homing_sps);
  }
  Motion_SetStepHz(s_step_hz);      /* frequence STEP timer (defaut 200 kHz) */
  servo_set(false);                 /* drivers coupes au demarrage           */
}

bool MC_ServoEnabled(void)
{
  return s_servo;
}

void MC_EnableConnectedMotors(void)
{
  if (s_restart_required) {
    DebugUart_Print("[MC] SERVO ON blocked: reboot required\r\n");
    return;
  }

  DebugUart_Print("[MC] SERVO ON (connected only)\r\n");
  for (uint8_t i = 0; i < MOTION_NUM_AXES; i++) {
    /* ENABLE inconditionnel (voir servo_set) : READY n'apparait qu'apres. */
    enable_write(&k_enable[i], s_motor[i].connected);
  }
  relais_write(true);
  s_servo = true;
}

void MC_DisableServo(void)
{
  servo_set(false);
}

void MC_LogEnabledStatus(void)
{
  for (uint8_t i = 0; i < MOTION_NUM_AXES; i++) {
    uint8_t connected = s_motor[i].connected ? 1u : 0u;
    uint8_t ready = ready_is_present(i) ? 1u : 0u;
    uint8_t enabled = connected; /* ENABLE suit desormais 'connected' seul */
    DebugUart_Printf("[MC] M%u conn=%u ready=%u enabled=%u\r\n",
                     (unsigned)(i + 1u),
                     (unsigned)connected,
                     (unsigned)ready,
                     (unsigned)enabled);
  }
}

void MC_LogEstopStatus(void)
{
  uint8_t estop_raw = estop_raw_level();
  uint8_t estop_active = estop_is_active();

  DebugUart_Printf("[MC] SH_START ESTOP mode=%s beh=%u pin_raw=%u pin_active=%u\r\n",
                   s_estop_enabled ? "ENABLED" : "DISABLED",
                   (unsigned)s_estop_behavior,
                   (unsigned)estop_raw,
                   (unsigned)estop_active);
}

bool MC_IsEstopMotionBlocked(void)
{
  return s_restart_required || (s_estop_enabled && (estop_is_active() != 0u));
}

bool MC_IsRestartRequired(void)
{
  return s_restart_required;
}

bool MC_RunMotorCalibration(uint8_t motor1Based)
{
  if (motor1Based < 1u || motor1Based > MOTION_NUM_AXES) {
    return false;
  }

  s_cal_fail_reason[0] = '\0';

  if (s_restart_required) {
    snprintf(s_cal_fail_reason, sizeof(s_cal_fail_reason), "REBOOT_REQUIRED");
    DebugUart_Print("[CAL] blocked: reboot required\r\n");
    return false;
  }

  /* Regle AVR (autoConnectIfNeeded) : une commande de calibration sur un
     moteur non connecte le connecte implicitement (RAM sur cette box). */
  {
    uint8_t idx = (uint8_t)(motor1Based - 1u);
    if (!s_motor[idx].connected) {
      s_motor[idx].connected = true;
      mc_cfg_touch();
      DebugUart_Printf("[CAL] M%u auto-connected for calibration\r\n",
                       (unsigned)motor1Based);
    }
  }

  MC_EnableConnectedMotors();
  Motion_SetMaxSps((uint32_t)s_homing_sps);   /* homing a vitesse reduite */
  {
    uint8_t bit = (uint8_t)(1u << (motor1Based - 1u));
    StatusLed_ShowDual(0u, 0u, 0u, 0u, bit, 255u, 96u, 0u);   /* LED moteur orange */
    CDC_SendStr("[ACT] COMPRESSING (searching MIN)\n");
    bool r = calibrate_axis_min((uint8_t)(motor1Based - 1u));
    if (r) {
      CDC_SendStr("MIN ENDSTOP FOUND\n");
    }
    Motion_SetMaxSps(0u);                     /* retour pleine vitesse    */
    StatusLed_ShowRgb(0u, 0u, 255u);          /* retour LED session MC    */
    return r;
  }
}

/* Calibration complete d'un verin optionnel (M5..M7) : homing MIN puis
   mesure de la course MAX (le max reel peut etre < 32767). Pour M1..M4,
   retombe sur le homing MIN seul (course fixe). */
bool MC_RunMotorFullCalibration(uint8_t motor1Based)
{
  if (motor1Based < 1u || motor1Based > MOTION_NUM_AXES) {
    return false;
  }
  if (motor1Based <= 4u) {
    return MC_RunMotorCalibration(motor1Based);
  }

  s_cal_fail_reason[0] = '\0';

  if (s_restart_required) {
    snprintf(s_cal_fail_reason, sizeof(s_cal_fail_reason), "REBOOT_REQUIRED");
    return false;
  }

  uint8_t idx = (uint8_t)(motor1Based - 1u);
  if (!s_motor[idx].connected) {
    s_motor[idx].connected = true;
    mc_cfg_touch();
    DebugUart_Printf("[CAL] M%u auto-connected for calibration\r\n",
                     (unsigned)motor1Based);
  }

  MC_EnableConnectedMotors();
  Motion_SetMaxSps((uint32_t)s_homing_sps);
  uint8_t bit = (uint8_t)(1u << idx);
  StatusLed_ShowDual(0u, 0u, 0u, 0u, bit, 255u, 96u, 0u);     /* LED moteur orange */

  /* La calibration complete se reference TOUJOURS au MIN (position 0),
     meme si la direction de homing de l'axe est reglee sur MAX. */
  uint8_t savedDir = s_motor[idx].homing_dir;
  s_motor[idx].homing_dir = HOMING_DIR_MIN;
  /* Marqueurs de phase pour Motion Center (animation + log). */
  CDC_SendStr("[ACT] COMPRESSING (searching MIN)\n");
  bool r = calibrate_axis_min(idx);
  if (r) {
    CDC_SendStr("MIN ENDSTOP FOUND\n");
    CDC_SendStr("[ACT] DECOMPRESSING (measuring MAX)\n");
    r = calibrate_axis_max(idx, true);
    if (r) {
      CDC_SendStr("MAX ENDSTOP FOUND\n");
    }
  }
  s_motor[idx].homing_dir = savedDir;

  Motion_SetMaxSps(0u);
  StatusLed_ShowRgb(0u, 0u, 255u);            /* retour LED session MC    */
  return r;
}

uint8_t MC_GetReadyMask(void)
{
  uint8_t mask = 0u;
  for (uint8_t i = 0; i < MOTION_NUM_AXES; i++) {
    if (ready_is_present(i)) {
      mask |= (uint8_t)(1u << i);
    }
  }
  return mask;
}
const char *MC_GetCalFailReason(void)
{
  return s_cal_fail_reason;
}

/* Applique le mapping produit (PosMap) a une consigne SimHub 15 bits (u :
   0..32767) pour l'axe idx : course utile bornee par le max/marge du moteur.
   M1..M4 : unites 15 bits (stepScale=1 sur G4). M5..M7 : pas physiques mesures. */
int32_t MC_MapAxisTarget(uint8_t idx, uint16_t u)
{
  if (idx >= MOTION_NUM_AXES) { return 0; }

  int32_t mx = s_motor[idx].max;
  if (mx <= 0)     { mx = MC_AXIS_MAX_STEPS; }
  if (mx > 0xFFFF) { mx = 0xFFFF; }
  int32_t mg = s_motor[idx].margin;
  if (mg < 0)      { mg = 0; }
  if (mg > 0xFFFF) { mg = 0xFFFF; }

  uint16_t target;
  if (idx < 4u) {
    target = PosMap_TargetBase(u, (uint16_t)mx, (uint16_t)mg, 1u);
  } else {
    target = PosMap_TargetOptional(u, (uint16_t)mx, (uint16_t)mg);
  }
  return (int32_t)target;
}

/* Deplace un axe vers 'target' (a la vitesse courante) et attend l'arrivee.
   false si timeout (proportionnel a la distance) ou ESTOP. */
static bool mc_move_to_wait(uint8_t idx, int32_t target)
{
  Motion_SetTarget(idx, target);
  int32_t delta = target - Motion_GetPosition(idx);
  if (delta < 0) { delta = -delta; }
  uint32_t timeoutMs = (uint32_t)delta * 1000u /
                       (s_homing_sps ? s_homing_sps : 1u) + 5000u;
  uint32_t t0 = HAL_GetTick();
  for (;;) {
    int32_t d = Motion_GetPosition(idx) - target;
    if (d < 0) { d = -d; }
    if (d <= MC_PARK_REACHED_TOL_STEPS) { return true; }
    if ((uint32_t)(HAL_GetTick() - t0) > timeoutMs) { return false; }
    if (MC_IsEstopMotionBlocked()) { return false; }
    HAL_Delay(MC_CAL_LOOP_DELAY_MS);
  }
}

/* Mise au centre "intelligente" d'un verin : si la position n'est pas connue
   dans la session (pas de homing), on re-reference d'abord sur la butee MIN,
   puis on rejoint le milieu de course (max/2) a vitesse de homing. Modele :
   goToCenterSmart() de la box AVR v2.1. */
bool MC_GoToCenter(uint8_t motor1Based)
{
  if (motor1Based < 1u || motor1Based > MOTION_NUM_AXES) {
    return false;
  }

  s_cal_fail_reason[0] = '\0';

  if (s_restart_required) {
    snprintf(s_cal_fail_reason, sizeof(s_cal_fail_reason), "REBOOT_REQUIRED");
    return false;
  }

  uint8_t idx = (uint8_t)(motor1Based - 1u);

  /* Auto-connexion (meme regle que la calibration). */
  if (!s_motor[idx].connected) {
    s_motor[idx].connected = true;
    mc_cfg_touch();
    DebugUart_Printf("[MC] M%u auto-connected for centering\r\n",
                     (unsigned)motor1Based);
  }

  MC_EnableConnectedMotors();

  uint8_t bit = (uint8_t)(1u << idx);
  StatusLed_ShowDual(0u, 0u, 0u, 0u, bit, 255u, 96u, 0u);   /* LED moteur orange */

  Motion_SetMaxSps((uint32_t)s_homing_sps);   /* deplacements a vitesse reduite */

  /* Position inconnue -> homing MIN prealable (comme sur la box AVR). */
  if (!s_motor[idx].homed) {
    CDC_SendStr("[ACT] COMPRESSING (searching MIN)\n");
    if (!calibrate_axis_min(idx)) {
      Motion_SetMaxSps(0u);
      StatusLed_ShowRgb(0u, 0u, 255u);
      return false;               /* raison posee par calibrate_axis_min */
    }
    CDC_SendStr("MIN ENDSTOP FOUND\n");
  }

  int32_t maxSteps = s_motor[idx].max;
  if (maxSteps <= 0) { maxSteps = MC_AXIS_MAX_STEPS; }
  int32_t center = maxSteps / 2;

  CDC_SendStr("[ACT] DECOMPRESSING (to center)\n");
  Motion_SetTarget(idx, center);

  int32_t delta = center - Motion_GetPosition(idx);
  if (delta < 0) { delta = -delta; }
  uint32_t timeoutMs = (uint32_t)delta * 1000u /
                       (s_homing_sps ? s_homing_sps : 1u) + 5000u;
  uint32_t t0 = HAL_GetTick();
  bool aborted = false;
  for (;;) {
    int32_t d = Motion_GetPosition(idx) - center;
    if (d < 0) { d = -d; }
    if (d <= MC_PARK_REACHED_TOL_STEPS) { break; }
    if ((uint32_t)(HAL_GetTick() - t0) > timeoutMs) {
      snprintf(s_cal_fail_reason, sizeof(s_cal_fail_reason),
               "M%u CENTER_TIMEOUT", (unsigned)motor1Based);
      aborted = true;
      break;
    }
    if (MC_IsEstopMotionBlocked()) {
      snprintf(s_cal_fail_reason, sizeof(s_cal_fail_reason),
               "M%u ESTOP_LOCK", (unsigned)motor1Based);
      aborted = true;
      break;
    }
    HAL_Delay(MC_CAL_LOOP_DELAY_MS);
  }

  Motion_SetMaxSps(0u);                       /* retour pleine vitesse */
  StatusLed_ShowRgb(0u, 0u, 255u);            /* retour LED session MC */

  if (aborted) {
    axis_stop_here(idx);
    return false;
  }

  DebugUart_Printf("[MC] M%u centered at %ld\r\n",
                   (unsigned)motor1Based, (long)center);
  return true;
}

/* Detection/mesure MAX standalone (bouton "Detecter max"). Re-reference MIN si
   la position n'est pas connue, puis mesure la course MAX (calibrate_axis_max,
   qui repose la reference et remonte STROKE Mx). */
bool MC_MoveToMax(uint8_t motor1Based)
{
  if (motor1Based < 1u || motor1Based > MOTION_NUM_AXES) { return false; }

  s_cal_fail_reason[0] = '\0';
  if (s_restart_required) {
    snprintf(s_cal_fail_reason, sizeof(s_cal_fail_reason), "REBOOT_REQUIRED");
    return false;
  }

  uint8_t idx = (uint8_t)(motor1Based - 1u);
  if (!s_motor[idx].connected) {
    s_motor[idx].connected = true;
    mc_cfg_touch();
    DebugUart_Printf("[MC] M%u auto-connected for MAX detect\r\n",
                     (unsigned)motor1Based);
  }

  MC_EnableConnectedMotors();
  uint8_t bit = (uint8_t)(1u << idx);
  StatusLed_ShowDual(0u, 0u, 0u, 0u, bit, 255u, 96u, 0u);   /* LED moteur orange */
  Motion_SetMaxSps((uint32_t)s_homing_sps);

  bool ok = true;
  /* Un max fiable exige la reference MIN (position 0 au min). */
  if (!s_motor[idx].homed) {
    CDC_SendStr("[ACT] COMPRESSING (searching MIN)\n");
    ok = calibrate_axis_min(idx);
    if (ok) { CDC_SendStr("MIN ENDSTOP FOUND\n"); }
  }
  if (ok) {
    CDC_SendStr("[ACT] DECOMPRESSING (measuring MAX)\n");
    ok = calibrate_axis_max(idx, false);   /* rester au max */
    if (ok) {
      s_motor[idx].homed = true;
      CDC_SendStr("MAX ENDSTOP FOUND\n");
    }
  }

  Motion_SetMaxSps(0u);
  StatusLed_ShowRgb(0u, 0u, 255u);
  return ok;
}

/* Test de course complete (bouton "Test course"). Re-reference MIN si besoin,
   puis min -> max -> centre, a vitesse de homing. Laisse le verin au centre. */
bool MC_TestStroke(uint8_t motor1Based)
{
  if (motor1Based < 1u || motor1Based > MOTION_NUM_AXES) { return false; }

  s_cal_fail_reason[0] = '\0';
  if (s_restart_required) {
    snprintf(s_cal_fail_reason, sizeof(s_cal_fail_reason), "REBOOT_REQUIRED");
    return false;
  }

  uint8_t idx = (uint8_t)(motor1Based - 1u);
  if (!s_motor[idx].connected) {
    s_motor[idx].connected = true;
    mc_cfg_touch();
    DebugUart_Printf("[MC] M%u auto-connected for stroke test\r\n",
                     (unsigned)motor1Based);
  }

  MC_EnableConnectedMotors();
  uint8_t bit = (uint8_t)(1u << idx);
  StatusLed_ShowDual(0u, 0u, 0u, 0u, bit, 255u, 96u, 0u);   /* LED moteur orange */
  Motion_SetMaxSps((uint32_t)s_homing_sps);

  bool ok = true;
  /* Course complete : toujours re-referencer MIN (modele AVR testFullStroke),
     meme si l'axe est deja home, pour parcourir toute la course. */
  CDC_SendStr("[ACT] COMPRESSING (searching MIN)\n");
  ok = calibrate_axis_min(idx);
  if (ok) { CDC_SendStr("MIN ENDSTOP FOUND\n"); }

  int32_t maxSteps = s_motor[idx].max;
  if (maxSteps <= 0) { maxSteps = MC_AXIS_MAX_STEPS; }

  if (ok) {
    CDC_SendStr("[ACT] DECOMPRESSING (to max)\n");
    ok = mc_move_to_wait(idx, maxSteps);
  }
  if (ok) {
    CDC_SendStr("[ACT] COMPRESSING (to center)\n");
    ok = mc_move_to_wait(idx, maxSteps / 2);
  }
  if (!ok && s_cal_fail_reason[0] == '\0') {
    snprintf(s_cal_fail_reason, sizeof(s_cal_fail_reason),
             "M%u STROKE_TIMEOUT", (unsigned)motor1Based);
  }

  Motion_SetMaxSps(0u);
  StatusLed_ShowRgb(0u, 0u, 255u);
  if (!ok) { axis_stop_here(idx); }
  return ok;
}

bool MC_RunStartupCalibration(void)
{
  bool ok = true;
  char msg[32];

  s_cal_fail_reason[0] = '\0';
  if (s_restart_required) {
    snprintf(s_cal_fail_reason, sizeof(s_cal_fail_reason), "REBOOT_REQUIRED");
    DebugUart_Print("[CAL] startup blocked: reboot required\r\n");
    return false;
  }

  MC_EnableConnectedMotors();
  Motion_SetMaxSps((uint32_t)s_homing_sps);   /* homing a vitesse reduite */
  {
    /* Progression LED par moteur : rouge = calibre, orange = en attente,
       eteint = non connecte. Chaque LED bascule des que son axe est fini. */
    uint8_t connMask = 0u;
    for (uint8_t i = 0; i < MOTION_NUM_AXES; i++) {
      if (s_motor[i].connected) { connMask |= (uint8_t)(1u << i); }
    }
    uint8_t doneMask = 0u;
    StatusLed_ShowDual(0u, 255u, 0u, 0u, connMask, 255u, 96u, 0u);
    for (uint8_t i = 0; i < MOTION_NUM_AXES; i++) {
      if (s_motor[i].connected) {
        snprintf(msg, sizeof(msg), "CalM%u\n", (unsigned)(i + 1u));
        CDC_SendStr(msg);
        if (!calibrate_axis_min(i)) {
          snprintf(msg, sizeof(msg), "CalM%u failed\n", (unsigned)(i + 1u));
          CDC_SendStr(msg);
          ok = false;
          break;
        }
        snprintf(msg, sizeof(msg), "CalM%u done\n", (unsigned)(i + 1u));
        CDC_SendStr(msg);
        doneMask |= (uint8_t)(1u << i);
        StatusLed_ShowDual(doneMask, 255u, 0u, 0u,
                           (uint8_t)(connMask & ~doneMask), 255u, 96u, 0u);
      }
    }
  }
  Motion_SetMaxSps(0u);                       /* retour pleine vitesse    */

  return ok;
}

/* Endpark (SH_DISABLE) : rejoint doucement s_motor[].endpark % de la course
   utile (max - 2*margin) sur les verins optionnels M5..M7 AVANT la coupure
   servo. Bloquant (comme la calibration SH_START) ; le stepping est fait par
   l'IRQ TIM6, cadence plafonnee a la vitesse de homing via Motion_SetMaxSps. */
void MC_RunEndPark(void)
{
  if (!s_servo || s_restart_required || MC_IsEstopMotionBlocked()) {
    return;
  }

  int32_t tgt[MOTION_NUM_AXES];
  bool    en[MOTION_NUM_AXES];
  int32_t maxDelta = 0;
  bool    any = false;

  for (uint8_t i = 0; i < MOTION_NUM_AXES; i++) {
    en[i] = false; tgt[i] = 0;
    if (i < 4u) { continue; }                       /* M5..M7 uniquement */
    if (!s_motor[i].connected) { continue; }
    if (s_motor[i].endpark > 100u) { continue; }    /* off */
    int32_t course = s_motor[i].max - 2 * s_motor[i].margin;
    if (course < 0) { course = s_motor[i].max; }
    int32_t t = (int32_t)(((int64_t)course * s_motor[i].endpark) / 100);
    if (t < 0) { t = 0; }
    if (t > s_motor[i].max) { t = s_motor[i].max; }
    tgt[i] = t;
    en[i]  = true;
    any    = true;
    int32_t d = Motion_GetPosition(i) - t;
    if (d < 0) { d = -d; }
    if (d > maxDelta) { maxDelta = d; }
  }
  if (!any) {
    return;
  }

  DebugUart_Print("[MC] SH_DISABLE endpark start\r\n");
  Motion_SetMaxSps((uint32_t)s_homing_sps);   /* "doucement" = vitesse homing */
  for (uint8_t i = 4u; i < MOTION_NUM_AXES; i++) {
    if (en[i]) { Motion_SetTarget(i, tgt[i]); }
  }

  const uint32_t timeoutMs =
      (uint32_t)maxDelta * 1000u / (s_homing_sps ? s_homing_sps : 1u) + 5000u;
  const uint32_t t0 = HAL_GetTick();
  for (;;) {
    bool pending = false;
    for (uint8_t i = 4u; i < MOTION_NUM_AXES; i++) {
      if (!en[i]) { continue; }
      int32_t d = Motion_GetPosition(i) - tgt[i];
      if (d < 0) { d = -d; }
      if (d > MC_PARK_REACHED_TOL_STEPS) { pending = true; break; }
    }
    if (!pending) { break; }
    if ((uint32_t)(HAL_GetTick() - t0) > timeoutMs) {
      DebugUart_Print("[MC] endpark timeout\r\n");
      break;
    }
    if (MC_IsEstopMotionBlocked()) {
      DebugUart_Print("[MC] endpark aborted (ESTOP)\r\n");
      break;
    }
    HAL_Delay(MC_CAL_LOOP_DELAY_MS);
  }
  Motion_SetMaxSps(0u);                       /* retour pleine vitesse    */
  DebugUart_Print("[MC] endpark done\r\n");
}

bool MC_HandleLine(const char *line)
{
  /* --- Detection / etat -------------------------------------------------- */
  if (is_cmd(line, "HELLO") || is_cmd(line, "MC_START")) {
    StatusLed_ShowRgb(0u, 0u, 255u);
    CDC_SendStr("OK_MC_CONNECTED\n");
    return true;
  }
  if (is_cmd(line, "GET")) {
    send_status();
    return true;
  }

  /* --- Activation drivers ------------------------------------------------ */
  if (is_cmd(line, "ENABLE")) {
    if (s_restart_required) {
      DebugUart_Print("[MC] CMD ENABLE blocked: reboot required\r\n");
      CDC_SendStr("ERR REBOOT_REQUIRED\n");
      return true;
    }
    DebugUart_Print("[MC] CMD ENABLE\r\n");
    servo_set(true);
    CDC_SendStr("OK\n");
    return true;
  }
  if (is_cmd(line, "DISABLE") || is_cmd(line, "MC_DISABLE")) {
    DebugUart_Print("[MC] CMD DISABLE\r\n");
    servo_set(false);
    CDC_SendStr("OK\n");
    return true;
  }
  if (is_cmd(line, "DISCONNECT") || is_cmd(line, "MC_STOP")) {
    servo_set(false);
    StatusLed_ShowPerMotor(MC_GetReadyMask(), 0u, 255u, 0u);
    return true;
  }

  /* --- Mise a jour firmware (DFU) ---------------------------------------- */
  if (is_cmd(line, "!DFU")) {
    enter_dfu();                    /* ne revient pas (reset)                */
    return true;
  }

  /* --- Mise a jour bootloader (BL_UPDATE) --------------------------------- */
  if (is_cmd(line, "!BL_UPDATE")) {
    DebugUart_Print("[MC] !BL_UPDATE -> entering bootloader update mode\r\n");
    mc_cfg_flush();
    BL_Update_Enter();              /* ne revient pas : mode BL update actif */
    return true;
  }

  /* --- Actions DO ... ---------------------------------------------------- */
  if (is_cmd(line, "DO")) {
    if (strstr(line, "SERVO_ON")) {
      if (s_restart_required) {
        DebugUart_Print("[MC] DO SERVO_ON blocked: reboot required\r\n");
        CDC_SendStr("ERR REBOOT_REQUIRED\n");
        return true;
      }
      servo_set(true);
      CDC_SendStr("OK\n");
      return true;
    }
    if (strstr(line, "SERVO_OFF")) { servo_set(false); CDC_SendStr("OK\n"); return true; }
    if (strstr(line, "FULL_CALIB") || strstr(line, "DETECT_MIN")) {
      bool full = (strstr(line, "FULL_CALIB") != NULL);
      int motor = 0;
      if (sscanf(line, "DO %*s %d", &motor) == 1 && motor >= 1 && motor <= (int)MOTION_NUM_AXES) {
        bool okc = full ? MC_RunMotorFullCalibration((uint8_t)motor)
                        : MC_RunMotorCalibration((uint8_t)motor);
        if (okc) {
          CDC_SendStr("OK\n");
        } else {
          /* Meme diagnostic que SH_START : raison detaillee AVANT le ERR
             (Mx MIN_TIMEOUT / MAX_TIMEOUT / ENDSTOP_STUCK / NO_READY...). */
          char msg[80];
          const char *why = MC_GetCalFailReason();
          snprintf(msg, sizeof(msg), "HOMING FAILED %s\n",
                   (why && why[0]) ? why : "UNKNOWN");
          CDC_SendStr(msg);
          CDC_SendStr("ERR CALIB_FAILED\n");
        }
      } else {
        CDC_SendStr("ERR\n");
      }
      return true;
    }
    if (strstr(line, "CENTER")) {
      int motor = 0;
      if (sscanf(line, "DO %*s %d", &motor) == 1 &&
          motor >= 1 && motor <= (int)MOTION_NUM_AXES) {
        if (MC_GoToCenter((uint8_t)motor)) {
          CDC_SendStr("OK\n");
        } else {
          /* Meme format de diagnostic que FULL_CALIB : raison AVANT le ERR. */
          char msg[80];
          const char *why = MC_GetCalFailReason();
          snprintf(msg, sizeof(msg), "HOMING FAILED %s\n",
                   (why && why[0]) ? why : "UNKNOWN");
          CDC_SendStr(msg);
          CDC_SendStr("ERR CENTER_FAILED\n");
        }
      } else {
        CDC_SendStr("ERR\n");
      }
      return true;
    }
    if (strstr(line, "MOVE_TO_MAX")) {
      int motor = 0;
      if (sscanf(line, "DO %*s %d", &motor) == 1 &&
          motor >= 1 && motor <= (int)MOTION_NUM_AXES) {
        if (MC_MoveToMax((uint8_t)motor)) {
          CDC_SendStr("OK\n");
        } else {
          char msg[80];
          const char *why = MC_GetCalFailReason();
          snprintf(msg, sizeof(msg), "HOMING FAILED %s\n",
                   (why && why[0]) ? why : "UNKNOWN");
          CDC_SendStr(msg);
          CDC_SendStr("ERR MAX_FAILED\n");
        }
      } else {
        CDC_SendStr("ERR\n");
      }
      return true;
    }
    if (strstr(line, "TEST_STROKE") || strstr(line, "STROKE")) {
      int motor = 0;
      if (sscanf(line, "DO %*s %d", &motor) == 1 &&
          motor >= 1 && motor <= (int)MOTION_NUM_AXES) {
        if (MC_TestStroke((uint8_t)motor)) {
          CDC_SendStr("OK\n");
        } else {
          char msg[80];
          const char *why = MC_GetCalFailReason();
          snprintf(msg, sizeof(msg), "HOMING FAILED %s\n",
                   (why && why[0]) ? why : "UNKNOWN");
          CDC_SendStr(msg);
          CDC_SendStr("ERR STROKE_FAILED\n");
        }
      } else {
        CDC_SendStr("ERR\n");
      }
      return true;
    }
    if (strstr(line, "FACTORY_RESET")) {
      mc_apply_default_motor_config();
      mc_apply_default_global_config();
      s_homing_sps = MC_HOMING_SPS;
      s_cfg_dirty = false;
      mc_cfg_save();
      CDC_SendStr("OK\n");
      return true;
    }
    /* FACTORY_RESET and other DO actions: remaining entries are still stubs. */
    CDC_SendStr("OK\n");
    return true;
  }

  /* --- Reglages SET <KEY> <VAL> [moteur] --------------------------------- */
  if (is_cmd(line, "SET")) {
    char key[16] = {0};
    long val = 0; int motor = 0;
    int got = sscanf(line, "SET %15s %ld %d", key, &val, &motor);
    if (got >= 2) {
      if (strcmp(key, "HOMING_S") == 0) {
        DebugUart_Printf("[MC] SET HOMING_S %ld\r\n", val);
        if (val < (long)MC_HOMING_SPS_MIN) { val = (long)MC_HOMING_SPS_MIN; }
        if (val > (long)MC_HOMING_SPS_MAX) { val = (long)MC_HOMING_SPS_MAX; }
        s_homing_sps = (uint16_t)val;
        mc_cfg_touch();
      } else if (strcmp(key, "STEP_HZ") == 0) {
        if (val < (long)MOTION_STEP_HZ_MIN) { val = (long)MOTION_STEP_HZ_MIN; }
        if (val > (long)MOTION_STEP_HZ_MAX) { val = (long)MOTION_STEP_HZ_MAX; }
        s_step_hz = (uint32_t)val;
        Motion_SetStepHz(s_step_hz);
        DebugUart_Printf("[MC] SET STEP_HZ %lu\r\n", (unsigned long)s_step_hz);
      } else if (strcmp(key, "ESTOP") == 0) {
        DebugUart_Printf("[MC] SET ESTOP %ld\r\n", val);
        s_estop_enabled = (val != 0);
        mc_cfg_touch();
        DebugUart_Printf("[MC] ESTOP mode %s\r\n",
                         s_estop_enabled ? "ENABLED" : "DISABLED");
        if (!s_estop_enabled) {
          s_estop_led_phase = 0u;
          s_estop_line_prev_low = 0u;
        }
      } else if (strcmp(key, "ESTOPB") == 0 || strcmp(key, "ESTOP_BEHAV") == 0) {
        uint8_t beh = (uint8_t)val;
        if (beh > MC_ESTOP_BEHAV_MAX) {
          beh = MC_ESTOP_BEHAV_DISABLE_SERVO;
        }
        s_estop_behavior = beh;
        mc_cfg_touch();
        DebugUart_Printf("[MC] ESTOP behavior %u\r\n", (unsigned)s_estop_behavior);
      } else if (got == 3 && motor >= 1 && motor <= (int)MOTION_NUM_AXES) {
        uint8_t idx = (uint8_t)(motor - 1);
        if      (strcmp(key, "CONNECTED") == 0) { s_motor[idx].connected = (val != 0); }
        else if (strcmp(key, "MAX")       == 0) {
          int32_t v = (int32_t)val;
          if (v < 0) { v = 0; }
          if (v > MC_AXIS_MAX_STEPS) { v = MC_AXIS_MAX_STEPS; }
          s_motor[idx].max = v;
        }
        else if (strcmp(key, "MARGIN")    == 0) { s_motor[idx].margin = (int32_t)val; }
        else if (strcmp(key, "HOMING_DIR") == 0) {
          /* 0=MIN, 1=MAX (shared/HomingDir.h). Homing MAX exige un max
             calibre ; la direction effective est re-testee au homing. */
          s_motor[idx].homing_dir = (val != 0) ? HOMING_DIR_MAX : HOMING_DIR_MIN;
        }
        else if (strcmp(key, "ENDPARK") == 0) {
          /* Endpark % (0..100, 255=off) : position rejointe doucement au
             SH_DISABLE avant la coupure servo. Verins optionnels M5..M7. */
          if (idx >= 4u) {
            s_motor[idx].endpark = (val >= 0 && val <= 100)
                                     ? (uint8_t)val : (uint8_t)MC_ENDPARK_OFF;
          }
        }
        mc_cfg_touch();
      }
    }
    CDC_SendStr("OK\n");
    return true;
  }

  /* --- Jog manuel STEP <moteur> <delta> ---------------------------------- */
  if (is_cmd(line, "STEP")) {
    if (s_restart_required) {
      CDC_SendStr("ERR REBOOT_REQUIRED\n");
      return true;
    }
    int m = 0; long delta = 0;
    if (sscanf(line, "STEP %d %ld", &m, &delta) == 2 &&
        m >= 1 && m <= (int)MOTION_NUM_AXES) {
      uint8_t idx = (uint8_t)(m - 1);
      Motion_SetTarget(idx, Motion_GetTarget(idx) + (int32_t)delta);
      CDC_SendStr("OK\n");
    } else {
      CDC_SendStr("ERR\n");
    }
    return true;
  }

  return false;                     /* non reconnue                          */
}
