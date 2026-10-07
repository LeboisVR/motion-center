/**
 ******************************************************************************
 * @file    motion.c
 * @brief   Moteur de mouvement generique 7 axes - implementation.
 *
 *  Principe (axes HW) :
 *    - Le generateur emet des pulses STEP en PWM continu tant qu'il tourne.
 *    - Le compteur (External Clock Mode 1) compte ces pulses via ITR.
 *      CR1.DIR selectionne le sens up/down -> la position suit le mouvement.
 *    - On charge CCR1 = vzero + cible et on arme l'IT Output Compare. Quand
 *      CNT atteint la cible, l'IT coupe le generateur : arret au pulse pres,
 *      CPU libre entre deux cibles.
 *    - Compteurs 16 bits (TIM1/20/15) : CCR1 ne couvre que 16 bits. On suit
 *      le mot haut en logiciel (IT Update) et on n'arme l'OC que lorsqu'on
 *      entre dans la fenetre 16 bits finale -> course illimitee.
 *
 *  Principe (axes bit-bang S6/S7) :
 *    - TIM6 @ 200 kHz : chaque IT bascule la broche STEP. 2 IT = 1 pulse
 *      (jusqu'a 100 kHz). Le sens (DIR) coute 1 tick de setup. Comptage
 *      logiciel dans l'IT.
 ******************************************************************************
 */
#include "motion.h"
#include "debug_uart.h"
#include "mc_protocol.h"

/* ------------------------------------------------------------------------- */
/*  Handles timers fournis par CubeMX (declares dans main.c).                */
/* ------------------------------------------------------------------------- */
extern TIM_HandleTypeDef htim3;   /* gen S1 */
extern TIM_HandleTypeDef htim2;   /* cnt S1 (32b) */
extern TIM_HandleTypeDef htim4;   /* gen S2 */
extern TIM_HandleTypeDef htim5;   /* cnt S2 (32b) */
extern TIM_HandleTypeDef htim8;   /* gen S3 */
extern TIM_HandleTypeDef htim1;   /* cnt S3 (16b) */
extern TIM_HandleTypeDef htim16;  /* gen S4 */
extern TIM_HandleTypeDef htim20;  /* cnt S4 (16b) */
extern TIM_HandleTypeDef htim17;  /* gen S5 */
extern TIM_HandleTypeDef htim15;  /* cnt S5 (16b) */
extern TIM_HandleTypeDef htim6;   /* tick bit-bang 200 kHz */

typedef struct {
	TIM_HandleTypeDef *genTim;
	uint32_t           genChannel;
	uint8_t            genMOE;
	TIM_HandleTypeDef *cntTim;
	uint32_t           cntChannel;
	uint8_t            width16;
	uint32_t           vzero;

	GPIO_TypeDef      *dirPort;
	uint16_t           dirPin;
	GPIO_TypeDef      *stepPort;
	uint16_t           stepPin;
	uint8_t            isBitBang;

	volatile int32_t   target;
	volatile int32_t   position;
	volatile int8_t    stepDir;
	volatile uint8_t   running;
	volatile uint8_t   ocArmed;
	volatile int32_t   cntHigh;
	int32_t            targetCnt;
	volatile uint8_t   bbPhase;
	volatile int8_t    bbDir;
	uint8_t            dirCr1Positive;
	int32_t            lastObsPos;
	uint8_t            wrongDirStreak;
	uint8_t            noMoveStreak;
	uint8_t            errGrowStreak;
} axis_t;

static axis_t g_ax[MOTION_NUM_AXES];
static uint8_t s_bb_tick_div_cnt;

/* Diviseur de cadence runtime (Motion_SetMaxSps) : 1 = pleine vitesse. */
static volatile uint16_t s_bb_user_div = 1u;
static uint16_t          s_bb_user_div_cnt;

/* Frequence STEP pleine vitesse des axes timer HW (streaming SimHub). */
static uint32_t s_hw_full_hz = MOTION_STEP_HZ_DEFAULT;

static void on_new_target(axis_t *a);

static void dir_setup_delay(void)
{
	for (volatile uint32_t i = 0; i < MOTION_DIR_SETUP_NOPS; i++) { __NOP(); }
}

static uint32_t cc_it_flag(uint32_t channel)
{
	switch (channel) {
		case TIM_CHANNEL_1: return TIM_IT_CC1;
		case TIM_CHANNEL_2: return TIM_IT_CC2;
		case TIM_CHANNEL_3: return TIM_IT_CC3;
		case TIM_CHANNEL_4: return TIM_IT_CC4;
		default:            return TIM_IT_CC1;
	}
}

static void gen_start(axis_t *a)
{
	TIM_CCxChannelCmd(a->genTim->Instance, a->genChannel, TIM_CCx_ENABLE);
	if (a->genMOE) { __HAL_TIM_MOE_ENABLE(a->genTim); }
	__HAL_TIM_ENABLE(a->genTim);
}

static void gen_stop(axis_t *a)
{
	__HAL_TIM_DISABLE(a->genTim);
	TIM_CCxChannelCmd(a->genTim->Instance, a->genChannel, TIM_CCx_DISABLE);
}

/* Programme la frequence PWM d'un generateur STEP (axe timer HW) pour ~hz pas/s.
   Sans effet sur les axes bit-bang. Le changement de PSC est latche par un
   event update (reset CNT : leger glitch, acceptable aux transitions de vitesse
   homing<->streaming ou l'axe est a l'arret / en debut de course). */
static void hw_gen_program_freq(axis_t *a, uint32_t hz)
{
	if (a->isBitBang || a->genTim == NULL) { return; }
	if (hz == 0u) { hz = 1u; }

	uint32_t counts = MOTION_STEP_GEN_CLK_HZ / hz;
	if (counts < 2u) { counts = 2u; }
	uint32_t psc = (counts - 1u) / 65536u;          /* garde ARR sur 16 bits */
	uint32_t arr = counts / (psc + 1u);
	if (arr < 2u) { arr = 2u; }
	arr -= 1u;
	if (arr > 65535u) { arr = 65535u; }

	TIM_TypeDef *T = a->genTim->Instance;
	uint32_t ccr = (arr + 1u) / 2u;                  /* rapport cyclique ~50%  */
	T->PSC = psc;
	T->ARR = arr;
	switch (a->genChannel) {
		case TIM_CHANNEL_2: T->CCR2 = ccr; break;
		case TIM_CHANNEL_3: T->CCR3 = ccr; break;
		case TIM_CHANNEL_4: T->CCR4 = ccr; break;
		case TIM_CHANNEL_1:
		default:            T->CCR1 = ccr; break;
	}
	T->EGR = TIM_EGR_UG;                             /* latch PSC              */
	T->SR &= ~TIM_SR_UIF;                            /* pas d'IT parasite      */
}

static int32_t hw_composite(axis_t *a)
{
	if (!a->width16) {
		return (int32_t)((uint32_t)a->cntTim->Instance->CNT - a->vzero);
	}

	uint32_t hi1, lo, hi2;
	do {
		hi1 = (uint32_t)a->cntHigh;
		lo  = (uint32_t)(a->cntTim->Instance->CNT & 0xFFFFu);
		hi2 = (uint32_t)a->cntHigh;
	} while (hi1 != hi2);
	return (int32_t)(((hi1 << 16) | lo) - a->vzero);
}

static void oc_arm(axis_t *a)
{
	if (!a->width16) {
		__HAL_TIM_SET_COMPARE(a->cntTim, a->cntChannel, (uint32_t)a->targetCnt);
		__HAL_TIM_CLEAR_FLAG(a->cntTim, cc_it_flag(a->cntChannel));
		__HAL_TIM_ENABLE_IT(a->cntTim, cc_it_flag(a->cntChannel));
		a->ocArmed = 1u;
		return;
	}

	uint32_t hiTarget = ((uint32_t)a->targetCnt >> 16) & 0xFFFFu;
	if ((uint32_t)a->cntHigh == hiTarget) {
		__HAL_TIM_SET_COMPARE(a->cntTim, a->cntChannel,
													(uint32_t)a->targetCnt & 0xFFFFu);
		__HAL_TIM_CLEAR_FLAG(a->cntTim, cc_it_flag(a->cntChannel));
		__HAL_TIM_ENABLE_IT(a->cntTim, cc_it_flag(a->cntChannel));
		a->ocArmed = 1u;
	} else {
		__HAL_TIM_DISABLE_IT(a->cntTim, cc_it_flag(a->cntChannel));
		a->ocArmed = 0u;
	}
}

static void oc_disarm(axis_t *a)
{
	__HAL_TIM_DISABLE_IT(a->cntTim, cc_it_flag(a->cntChannel));
	a->ocArmed = 0u;
}

static void set_counter_dir_for_step(axis_t *a, int8_t nd)
{
	uint8_t setDirBit;
	if (nd > 0) {
		setDirBit = a->dirCr1Positive;
	} else {
		setDirBit = (uint8_t)!a->dirCr1Positive;
	}

	if (setDirBit) { a->cntTim->Instance->CR1 |= TIM_CR1_DIR; }
	else           { a->cntTim->Instance->CR1 &= ~TIM_CR1_DIR; }
}

static void counter_reset_to_zero(axis_t *a)
{
	if (a->width16) {
		a->cntTim->Instance->CNT = 0x0000u;
		a->cntHigh = (int32_t)MOTION_VZERO_HI16;
	} else {
		a->cntTim->Instance->CNT = MOTION_VZERO;
		a->cntHigh = 0;
	}
	a->position = 0;
	a->target = 0;
	a->stepDir = 0;
}

static void counter_set_composite(axis_t *a, int32_t pos)
{
	uint32_t v = a->vzero + (uint32_t)pos;
	if (a->width16) {
		a->cntHigh = (int32_t)((v >> 16) & 0xFFFFu);
		a->cntTim->Instance->CNT = (uint16_t)(v & 0xFFFFu);
	} else {
		a->cntTim->Instance->CNT = v;
	}
	a->position = pos;
	a->target = pos;
	a->stepDir = 0;
	a->lastObsPos = pos;
	a->wrongDirStreak = 0u;
	a->noMoveStreak = 0u;
	a->errGrowStreak = 0u;
}

static void safety_stop_and_reanchor(axis_t *a)
{
	gen_stop(a);
	a->running = 0u;
	oc_disarm(a);
	counter_set_composite(a, a->target);
}

static void motion_watchdog_update(axis_t *a, uint8_t axisIdx)
{
	int32_t pos = hw_composite(a);
	int32_t delta = pos - a->lastObsPos;
	int32_t err = a->target - pos;
	int32_t prevErr = a->target - a->lastObsPos;
	int32_t absErr = (err >= 0) ? err : -err;
	int32_t absPrevErr = (prevErr >= 0) ? prevErr : -prevErr;

	if (a->running) {
		if ((a->stepDir > 0 && pos > a->target) ||
				(a->stepDir < 0 && pos < a->target)) {
			DebugUart_Printf("[MOT] AX%u watchdog overshoot, stop\r\n",
			                 (unsigned)(axisIdx + 1u));
			safety_stop_and_reanchor(a);
			pos = hw_composite(a);
			err = a->target - pos;
			absErr = (err >= 0) ? err : -err;
		}

		if (delta == 0) {
			a->noMoveStreak++;
		} else {
			a->noMoveStreak = 0u;
		}

		if ((delta > 0 && a->stepDir < 0) || (delta < 0 && a->stepDir > 0)) {
			a->wrongDirStreak++;
		} else if (delta != 0) {
			a->wrongDirStreak = 0u;
		}

		if (a->wrongDirStreak >= 3u) {
			a->dirCr1Positive = (uint8_t)!a->dirCr1Positive;
			DebugUart_Printf("[MOT] AX%u watchdog wrong-dir, flip dir map\r\n",
			                 (unsigned)(axisIdx + 1u));
			safety_stop_and_reanchor(a);
			pos = hw_composite(a);
		}

		if (a->noMoveStreak >= 8u) {
			DebugUart_Printf("[MOT] AX%u watchdog no-count, stop\r\n",
			                 (unsigned)(axisIdx + 1u));
			safety_stop_and_reanchor(a);
			pos = hw_composite(a);
			err = a->target - pos;
			absErr = (err >= 0) ? err : -err;
		}

		if (absErr > (absPrevErr + 64)) {
			a->errGrowStreak++;
		} else if (absErr + 64 < absPrevErr) {
			a->errGrowStreak = 0u;
		}

		if (a->errGrowStreak >= 4u || absErr > 120000) {
			DebugUart_Printf("[MOT] AX%u watchdog growing-error, stop\r\n",
			                 (unsigned)(axisIdx + 1u));
			safety_stop_and_reanchor(a);
			pos = hw_composite(a);
			a->errGrowStreak = 0u;
		}
	}

	a->position = pos;
	a->lastObsPos = pos;
}

static int32_t probe_target_sign(axis_t *a, uint8_t dirPositiveBit, int32_t target)
{
	a->dirCr1Positive = dirPositiveBit;
	counter_reset_to_zero(a);
	a->target = target;
	on_new_target(a);
	HAL_Delay(3);
	gen_stop(a);
	a->running = 0u;
	oc_disarm(a);

	return hw_composite(a);
}

static void detect_counter_dir_polarity(axis_t *a, uint8_t axisIdx)
{
	int32_t p0 = probe_target_sign(a, 0u, +256);
	int32_t n0 = probe_target_sign(a, 0u, -256);
	int32_t p1 = probe_target_sign(a, 1u, +256);
	int32_t n1 = probe_target_sign(a, 1u, -256);

	uint8_t ok0 = (uint8_t)((p0 > 0) && (n0 < 0));
	uint8_t ok1 = (uint8_t)((p1 > 0) && (n1 < 0));

	if (ok0 && !ok1) {
		a->dirCr1Positive = 0u;
	} else if (ok1 && !ok0) {
		a->dirCr1Positive = 1u;
	} else {
		/* Fallback: keep previous default if probe is inconclusive. */
		a->dirCr1Positive = 0u;
	}

	DebugUart_Printf("[MOT] AX%u dirProbe p0=%ld n0=%ld p1=%ld n1=%ld dirPosBit=%u\r\n",
	                 (unsigned)(axisIdx + 1u),
	                 (long)p0, (long)n0, (long)p1, (long)n1,
	                 (unsigned)a->dirCr1Positive);

	counter_reset_to_zero(a);
}

static void on_new_target(axis_t *a)
{
	int32_t pos = hw_composite(a);

	if (!MC_ServoEnabled()) {
		if (a->running) { gen_stop(a); a->running = 0u; }
		oc_disarm(a);
		a->target = pos;
		return;
	}

	/* Safety: if counter position drifts far from the 15-bit command space,
	   re-anchor to the current target to avoid full-speed runaway. */
	if (pos > 100000 || pos < -100000) {
		if (a->running) { gen_stop(a); a->running = 0u; }
		oc_disarm(a);
		counter_set_composite(a, a->target);
		return;
	}

	if (a->target == pos) {
		if (a->running) { gen_stop(a); a->running = 0u; }
		oc_disarm(a);
		return;
	}

	int8_t nd = (a->target > pos) ? (int8_t)+1 : (int8_t)-1;
	if (nd != a->stepDir) {
		HAL_GPIO_WritePin(a->dirPort, a->dirPin,
											(nd > 0) ? GPIO_PIN_SET : GPIO_PIN_RESET);
		set_counter_dir_for_step(a, nd);
		a->stepDir = nd;
		dir_setup_delay();
	}

	a->targetCnt = (int32_t)(a->vzero + (uint32_t)a->target);
	oc_arm(a);

	if (!a->running) { gen_start(a); a->running = 1u; }
}

static void bitbang_tick(void)
{
	if (!MC_ServoEnabled()) {
		for (uint8_t i = 0u; i < MOTION_NUM_AXES; i++) {
			axis_t *a = &g_ax[i];
			if (!a->isBitBang) { continue; }
			a->target = a->position;
			a->bbPhase = 0u;
			a->stepPort->BRR = a->stepPin;
		}
		return;
	}

	if (MOTION_BITBANG_TICK_DIV > 1u) {
		s_bb_tick_div_cnt++;
		if (s_bb_tick_div_cnt < MOTION_BITBANG_TICK_DIV) {
			return;
		}
		s_bb_tick_div_cnt = 0u;
	}

	/* Plafond de vitesse runtime (homing/calibration). */
	if (s_bb_user_div > 1u) {
		s_bb_user_div_cnt++;
		if (s_bb_user_div_cnt < s_bb_user_div) {
			return;
		}
		s_bb_user_div_cnt = 0u;
	}

	for (uint8_t i = 0u; i < MOTION_NUM_AXES; i++) {
		axis_t *a = &g_ax[i];
		if (!a->isBitBang) { continue; }

		if (a->bbPhase == 0u) {
			if (a->position == a->target) { continue; }
			int8_t nd = (a->target > a->position) ? (int8_t)+1 : (int8_t)-1;
			if (nd != a->bbDir) {
				a->bbDir = nd;
				HAL_GPIO_WritePin(a->dirPort, a->dirPin,
													(nd > 0) ? GPIO_PIN_SET : GPIO_PIN_RESET);
				continue;
			}
			a->stepPort->BSRR = a->stepPin;
			a->bbPhase = 1u;
		} else {
			a->stepPort->BRR = a->stepPin;
			a->bbPhase = 0u;
			a->position += a->bbDir;
		}
	}
}

void HAL_TIM_OC_DelayElapsedCallback(TIM_HandleTypeDef *htim)
{
	for (uint8_t i = 0; i < MOTION_NUM_HS_AXES; i++) {
		axis_t *a = &g_ax[i];
		if (a->cntTim->Instance == htim->Instance && a->ocArmed) {
			gen_stop(a);
			a->running = 0u;
			oc_disarm(a);
			a->position = hw_composite(a);
			return;
		}
	}
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
	if (htim->Instance == TIM6) {
		bitbang_tick();
		return;
	}

	for (uint8_t i = 0; i < MOTION_NUM_HS_AXES; i++) {
		axis_t *a = &g_ax[i];
		if (!a->width16) { continue; }
		if (a->cntTim->Instance != htim->Instance) { continue; }

		if ((((htim->Instance->CR1 & TIM_CR1_DIR) != 0u) ? 1u : 0u) == a->dirCr1Positive) {
			a->cntHigh++;
		} else {
			a->cntHigh--;
		}

		if (a->running && !a->ocArmed) {
			uint32_t hiTarget = ((uint32_t)a->targetCnt >> 16) & 0xFFFFu;
			if ((uint32_t)a->cntHigh == hiTarget) {
				__HAL_TIM_SET_COMPARE(a->cntTim, a->cntChannel,
															(uint32_t)a->targetCnt & 0xFFFFu);
				__HAL_TIM_CLEAR_FLAG(a->cntTim, cc_it_flag(a->cntChannel));
				__HAL_TIM_ENABLE_IT(a->cntTim, cc_it_flag(a->cntChannel));
				a->ocArmed = 1u;
			}
		}
		return;
	}
}

static void axes_config(void)
{
	g_ax[AX_S1] = (axis_t){
		.genTim = &htim3, .genChannel = TIM_CHANNEL_1, .genMOE = 0u,
		.cntTim = &htim2, .cntChannel = TIM_CHANNEL_1, .width16 = 0u,
		.vzero = MOTION_VZERO, .dirPort = D1_GPIO_Port, .dirPin = D1_Pin,
		.stepPort = S1_GPIO_Port, .stepPin = S1_Pin,
		.isBitBang = MOTION_FORCE_BITBANG_ALL ? 1u : 0u };

	g_ax[AX_S2] = (axis_t){
		.genTim = &htim4, .genChannel = TIM_CHANNEL_1, .genMOE = 0u,
		.cntTim = &htim5, .cntChannel = TIM_CHANNEL_1, .width16 = 0u,
		.vzero = MOTION_VZERO, .dirPort = D2_GPIO_Port, .dirPin = D2_Pin,
		.stepPort = S2_GPIO_Port, .stepPin = S2_Pin,
		.isBitBang = MOTION_FORCE_BITBANG_ALL ? 1u : 0u };

	g_ax[AX_S3] = (axis_t){
		.genTim = &htim8, .genChannel = TIM_CHANNEL_1, .genMOE = 1u,
		.cntTim = &htim1, .cntChannel = TIM_CHANNEL_1, .width16 = 1u,
		.vzero = MOTION_VZERO, .dirPort = D3_GPIO_Port, .dirPin = D3_Pin,
		.stepPort = S3_GPIO_Port, .stepPin = S3_Pin,
		.isBitBang = MOTION_FORCE_BITBANG_ALL ? 1u : 0u };

	g_ax[AX_S4] = (axis_t){
		.genTim = &htim16, .genChannel = TIM_CHANNEL_1, .genMOE = 1u,
		.cntTim = &htim20, .cntChannel = TIM_CHANNEL_1, .width16 = 1u,
		.vzero = MOTION_VZERO, .dirPort = D4_GPIO_Port, .dirPin = D4_Pin,
		.stepPort = S4_GPIO_Port, .stepPin = S4_Pin,
		.isBitBang = MOTION_FORCE_BITBANG_ALL ? 1u : 0u };

	g_ax[AX_S5] = (axis_t){
		.genTim = &htim17, .genChannel = TIM_CHANNEL_1, .genMOE = 1u,
		.cntTim = &htim15, .cntChannel = TIM_CHANNEL_1, .width16 = 1u,
		.vzero = MOTION_VZERO, .dirPort = D5_GPIO_Port, .dirPin = D5_Pin,
		.stepPort = S5_GPIO_Port, .stepPin = S5_Pin,
		.isBitBang = MOTION_FORCE_BITBANG_ALL ? 1u : 0u };

	g_ax[AX_S6] = (axis_t){
		.isBitBang = 1u, .vzero = MOTION_VZERO,
		.dirPort = D6_GPIO_Port, .dirPin = D6_Pin,
		.stepPort = S6_GPIO_Port, .stepPin = S6_Pin };

	g_ax[AX_S7] = (axis_t){
		.isBitBang = 1u, .vzero = MOTION_VZERO,
		.dirPort = D7_GPIO_Port, .dirPin = D7_Pin,
		.stepPort = S7_GPIO_Port, .stepPin = S7_Pin };
}

static void counter_prepare(axis_t *a)
{
	if (a->width16) {
		a->cntTim->Instance->ARR = 0xFFFFu;
		a->cntTim->Instance->CNT = 0x0000u;
		a->cntHigh = (int32_t)MOTION_VZERO_HI16;
	} else {
		a->cntTim->Instance->ARR = 0xFFFFFFFFu;
		a->cntTim->Instance->CNT = MOTION_VZERO;
		a->cntHigh = 0;
	}
	a->cntTim->Instance->CR1 &= ~TIM_CR1_DIR;
	__HAL_TIM_DISABLE_IT(a->cntTim, cc_it_flag(a->cntChannel));

	if (a->width16) {
		__HAL_TIM_CLEAR_FLAG(a->cntTim, TIM_FLAG_UPDATE);
		__HAL_TIM_ENABLE_IT(a->cntTim, TIM_IT_UPDATE);
	}

	__HAL_TIM_ENABLE(a->cntTim);
}

void Motion_Init(void)
{
	axes_config();
	s_bb_tick_div_cnt = 0u;

	for (uint8_t i = 0; i < MOTION_NUM_AXES; i++) {
		axis_t *a = &g_ax[i];

		if (a->isBitBang) {
			if (a->genTim != NULL) {
				gen_stop(a);
			}
			if (a->cntTim != NULL) {
				__HAL_TIM_DISABLE(a->cntTim);
				oc_disarm(a);
			}

			GPIO_InitTypeDef gpio = {0};
			gpio.Pin = a->stepPin;
			gpio.Mode = GPIO_MODE_OUTPUT_PP;
			gpio.Pull = GPIO_NOPULL;
			gpio.Speed = GPIO_SPEED_FREQ_HIGH;
			HAL_GPIO_Init(a->stepPort, &gpio);

			a->target   = 0;
			a->position = 0;
			a->bbPhase  = 0u;
			a->bbDir    = 0;
			HAL_GPIO_WritePin(a->stepPort, a->stepPin, GPIO_PIN_RESET);
			continue;
		}

		gen_stop(a);
		a->running = 0u;
		a->ocArmed = 0u;
		a->stepDir = 0;
		a->dirCr1Positive = 0u;
		a->lastObsPos = 0;
		a->wrongDirStreak = 0u;
		a->noMoveStreak = 0u;
		a->errGrowStreak = 0u;
		counter_prepare(a);
		if (MOTION_ENABLE_DIR_PROBE) {
			detect_counter_dir_polarity(a, i);
		} else {
			counter_reset_to_zero(a);
			DebugUart_Printf("[MOT] AX%u dirProbe skipped dirPosBit=%u\r\n",
			                 (unsigned)(i + 1u),
			                 (unsigned)a->dirCr1Positive);
		}
		a->target   = 0;
		a->position = 0;
	}

	for (uint8_t i = 0; i < MOTION_NUM_AXES; i++) {
		hw_gen_program_freq(&g_ax[i], s_hw_full_hz);
	}

	HAL_TIM_Base_Start_IT(&htim6);
}

void Motion_SetMaxSps(uint32_t sps)
{
	uint32_t div = 1u;
	if (sps > 0u) {
		div = s_hw_full_hz / sps;          /* base = cadence STEP courante */
		if (div == 0u)     { div = 1u; }
		if (div > 65535u)  { div = 65535u; }
	}
	s_bb_user_div_cnt = 0u;
	s_bb_user_div = (uint16_t)div;

	/* Axes timer HW (si actifs) : regler la frequence des generateurs. */
	uint32_t hz = (sps == 0u) ? s_hw_full_hz : sps;
	if (hz > s_hw_full_hz) { hz = s_hw_full_hz; }
	for (uint8_t i = 0u; i < MOTION_NUM_AXES; i++) {
		hw_gen_program_freq(&g_ax[i], hz);
	}
}

void Motion_SetStepHz(uint32_t hz)
{
	if (hz < MOTION_STEP_HZ_MIN) { hz = MOTION_STEP_HZ_MIN; }
	if (hz > MOTION_STEP_HZ_MAX) { hz = MOTION_STEP_HZ_MAX; }
	s_hw_full_hz = hz;

	/* Cadence commune TIM6 (2 ticks = 1 pulse) : tick = 2*hz. */
	{
		uint32_t period = MOTION_STEP_GEN_CLK_HZ / (2u * hz);
		if (period < 2u) { period = 2u; }
		if (period > 65536u) { period = 65536u; }
		htim6.Instance->PSC = 0u;
		htim6.Instance->ARR = period - 1u;
		htim6.Instance->EGR = TIM_EGR_UG;   /* latch immediat */
		__HAL_TIM_CLEAR_FLAG(&htim6, TIM_FLAG_UPDATE); /* pas d'IT parasite */
	}

	/* Axes timer HW (si actifs un jour) : meme frequence pleine vitesse. */
	for (uint8_t i = 0u; i < MOTION_NUM_AXES; i++) {
		hw_gen_program_freq(&g_ax[i], s_hw_full_hz);
	}
}

void Motion_SetTarget(uint8_t axis, int32_t steps)
{
	if (axis >= MOTION_NUM_AXES) { return; }
	axis_t *a = &g_ax[axis];

	if (!a->isBitBang) {
		motion_watchdog_update(a, axis);
	}

	if (a->target == steps) { return; }
	
	a->target = steps;

	if (a->isBitBang) { return; }
	on_new_target(a);
}

int32_t Motion_GetPosition(uint8_t axis)
{
	if (axis >= MOTION_NUM_AXES) { return 0; }
	axis_t *a = &g_ax[axis];
	if (a->isBitBang) { return a->position; }
	return hw_composite(a);
}

int32_t Motion_GetTarget(uint8_t axis)
{
	if (axis >= MOTION_NUM_AXES) { return 0; }
	return g_ax[axis].target;
}

void Motion_StopAll(void)
{
	for (uint8_t i = 0; i < MOTION_NUM_AXES; i++) {
		axis_t *a = &g_ax[i];
		if (a->isBitBang) {
			a->target = a->position;
			a->bbPhase = 0u;
			a->stepPort->BRR = a->stepPin;
			continue;
		}

		gen_stop(a);
		a->running = 0u;
		oc_disarm(a);
		a->target = hw_composite(a);
	}
}

void Motion_ZeroHere(uint8_t axis)
{
	if (axis >= MOTION_NUM_AXES) { return; }
	axis_t *a = &g_ax[axis];

	if (a->isBitBang) {
		a->position = 0;
		a->target   = 0;
		return;
	}

	__HAL_TIM_DISABLE_IT(a->cntTim, cc_it_flag(a->cntChannel));
	a->ocArmed = 0u;
	if (a->width16) {
		a->cntTim->Instance->CNT = 0x0000u;
		a->cntHigh = (int32_t)MOTION_VZERO_HI16;
	} else {
		a->cntTim->Instance->CNT = MOTION_VZERO;
		a->cntHigh = 0;
	}
	a->target  = 0;
	a->stepDir = 0;
}

void Motion_SetHere(uint8_t axis, int32_t pos)
{
	if (axis >= MOTION_NUM_AXES) { return; }
	axis_t *a = &g_ax[axis];

	if (a->isBitBang) {
		a->position = pos;
		a->target   = pos;
		return;
	}

	/* Axes HW timer : le recalage compteur arbitraire n'est pas supporte,
	   repli sur le zero (tous les axes produit sont en bit-bang :
	   MOTION_FORCE_BITBANG_ALL=1). */
	Motion_ZeroHere(axis);
}


