/**
 ******************************************************************************
 * @file    simhub_parser.c
 * @brief   Parser protocole SimHub sur USB CDC — implementation.
 ******************************************************************************
 */
#include "simhub_parser.h"
#include "motion.h"
#include "debug_uart.h"
#include "cdc_tx.h"
#include "mc_protocol.h"
#include "status_led.h"
#include "PFrame.h"   /* format commun trame 'P' (7 axes, 15 octets) */
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------------- */
/*  Ring buffer (rempli en IT USB, vide en boucle principale)                */
/* ------------------------------------------------------------------------- */
#define RING_SIZE   512u                    /* puissance de 2                 */
#define RING_MASK   (RING_SIZE - 1u)
static volatile uint8_t  s_ring[RING_SIZE];
static volatile uint16_t s_head;            /* ecriture (IT)                  */
static volatile uint16_t s_tail;            /* lecture (loop)                 */

/* ------------------------------------------------------------------------- */
/*  Stash de decodage (boucle principale uniquement)                         */
/* ------------------------------------------------------------------------- */
#define STASH_SIZE  256u
static uint8_t  s_stash[STASH_SIZE];
static uint16_t s_len;
static volatile uint32_t s_frames_decoded;
static volatile uint32_t s_text_lines;

/* ------------------------------------------------------------------------- */
/*  Helpers                                                                  */
/* ------------------------------------------------------------------------- */
static inline uint16_t be16(const uint8_t *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

/* Legacy stream observed on some hosts: 'P' + 7x uint16 BE (no terminator).
   Format commun shared/PFrame.h ; valeurs 15 bits (0..32767). */

/* Avance le stash de n octets. */
static void stash_consume(uint16_t n)
{
  if (n >= s_len) { s_len = 0u; return; }
  memmove(s_stash, s_stash + n, (size_t)(s_len - n));
  s_len = (uint16_t)(s_len - n);
}

/* ------------------------------------------------------------------------- */
/*  Drain ring -> stash                                                      */
/* ------------------------------------------------------------------------- */
static void drain_ring(void)
{
  while (s_len < STASH_SIZE) {
    uint16_t tail = s_tail;
    if (tail == s_head) { break; }          /* ring vide                     */
    s_stash[s_len++] = s_ring[tail];
    s_tail = (uint16_t)((tail + 1u) & RING_MASK);
  }
}

/* ------------------------------------------------------------------------- */
/*  Decodage texte (handshake)                                               */
/* ------------------------------------------------------------------------- */
static void handle_text_line(const char *line)
{
  /* Startup calibration flow:
     SH_START -> LED orange -> per-axis homing -> LED red -> CALIBRATED. */
  if (strcmp(line, "SH_START") == 0) {
    if (MC_IsRestartRequired()) {
      CDC_SendStr("ERR REBOOT_REQUIRED\n");
      return;
    }

    MC_LogEstopStatus();
    StatusLed_ShowRgb(255u, 96u, 0u);

    if (!MC_RunStartupCalibration()) {
      char msg[80];
      const char *why = MC_GetCalFailReason();
      snprintf(msg, sizeof(msg), "HOMING FAILED %s\n",
               (why && why[0]) ? why : "UNKNOWN");
      CDC_SendStr(msg);
      CDC_SendStr("ERR HOMING_FAILED\n");
      return;
    }

    MC_LogEnabledStatus();

    /* Phase active : rouge uniquement pour les moteurs READY (drivers
       alimentes et prets) ; les autres LEDs sont eteintes. */
    StatusLed_ShowPerMotor(MC_GetReadyMask(), 255u, 0u, 0u);
    CDC_SendStr("CALIBRATED\n");
     /* Some hosts occasionally miss a short CDC line right after long
       calibration traffic; repeat once so SH_START handshake is resilient. */
     HAL_Delay(10u);
     CDC_SendStr("CALIBRATED\n");
    return;
  }
  /* SH_DISABLE -> SH_DISABLED (arret SimHub).
     Endpark d'abord : rejoindre doucement le % de course configure
     (M5..M7), puis la procedure d'arret existante. */
  if (strcmp(line, "SH_DISABLE") == 0) {
    MC_RunEndPark();
    MC_DisableServo();
    Motion_StopAll();
    CDC_SendStr("SH_DISABLED\n");
    return;
  }
  /* Autres lignes : commandes Motion Center (handshake, GET, ENABLE, !DFU...). */
  (void)MC_HandleLine(line);
}

/* ------------------------------------------------------------------------- */
/*  API                                                                      */
/* ------------------------------------------------------------------------- */
void SimHub_Init(void)
{
  s_head = 0u;
  s_tail = 0u;
  s_len  = 0u;
  s_frames_decoded = 0u;
  s_text_lines = 0u;
}

void SimHub_RxPush(const uint8_t *buf, uint32_t len)
{
  for (uint32_t i = 0; i < len; i++) {
    uint16_t next = (uint16_t)((s_head + 1u) & RING_MASK);
    if (next == s_tail) { break; }          /* ring plein : on jette le reste */
    s_ring[s_head] = buf[i];
    s_head = next;
  }
}

void SimHub_Process(void)
{
  drain_ring();

  for (;;) {
    if (s_len == 0u) { break; }
    uint8_t c0 = s_stash[0];

    /* ===== Trame cible : 'T' + N x uint16 BE + '\n' ===================== */
    if (c0 == SH_FRAME_TAG) {
      if (s_len < SH_FRAME_LEN) { break; }  /* trame incomplete : attendre   */

      if (s_stash[SH_FRAME_TERM_IDX] != SH_FRAME_TERM) {
        stash_consume(1u);                  /* resync : consommer 1 octet     */
        continue;
      }

      if (MC_IsEstopMotionBlocked()) {
        s_frames_decoded++;
        stash_consume(SH_FRAME_LEN);
        continue;
      }

      const uint8_t *payload = &s_stash[1];
      
      for (uint8_t i = 0; i < MOTION_NUM_AXES; i++) {
        uint16_t raw = be16(&payload[2u * i]);
        Motion_SetTarget(i, MC_MapAxisTarget(i, (uint16_t)(raw & POSMAP_INPUT_MAX)));
      }
      s_frames_decoded++;
      stash_consume(SH_FRAME_LEN);
      continue;
    }

    /* ===== Trame legacy : format commun shared/PFrame.h ================ */
    if (c0 == PFRAME_TAG) {
      if (s_len < PFRAME_LEN) { break; }

      if (MC_IsEstopMotionBlocked()) {
        s_frames_decoded++;
        stash_consume(PFRAME_LEN);
        continue;
      }

      const uint8_t *payload = &s_stash[1];

      for (uint8_t i = 0; i < MOTION_NUM_AXES && i < PFRAME_AXES; i++) {
        Motion_SetTarget(i, MC_MapAxisTarget(i, PFrame_Axis(payload, i)));
      }
      s_frames_decoded++;
      stash_consume(PFRAME_LEN);
      continue;
    }

    /* ===== Ligne texte (handshake) ===================================== */
    if (c0 < 0x20u || c0 > 0x7Eu) {
      stash_consume(1u);
      continue;
    }

    /* Chercher un terminateur de ligne. */
    uint16_t nl = 0u;
    bool eol = false;
    while (nl < s_len) {
      uint8_t c = s_stash[nl];
      if (c == '\n' || c == '\r') { eol = true; break; }
      nl++;
    }
    if (!eol) {
      /* SimHub peut envoyer SH_START/SH_DISABLE sans terminateur. */
      if (s_len >= 8u && memcmp(s_stash, "SH_START", 8u) == 0) {
        if (s_len == 8u || s_stash[8] < 0x20u || s_stash[8] > 0x7Eu) {
          stash_consume(8u);
          s_text_lines++;
          handle_text_line("SH_START");
          continue;
        }
      }
      if (s_len >= 10u && memcmp(s_stash, "SH_DISABLE", 10u) == 0) {
        if (s_len == 10u || s_stash[10] < 0x20u || s_stash[10] > 0x7Eu) {
          stash_consume(10u);
          s_text_lines++;
          handle_text_line("SH_DISABLE");
          continue;
        }
      }

      /* Pas de terminateur : si le stash est plein on purge, sinon on attend. */
      if (s_len >= (uint16_t)(STASH_SIZE - 1u)) { s_len = 0u; }
      break;
    }

    char line[32];
    uint16_t n = nl;
    if (n > (uint16_t)(sizeof(line) - 1u)) { n = (uint16_t)(sizeof(line) - 1u); }
    memcpy(line, s_stash, n);
    line[n] = '\0';

    /* Consommer la ligne + terminateur(s). */
    uint16_t adv = (uint16_t)(nl + 1u);
    if (adv < s_len && (s_stash[adv] == '\n' || s_stash[adv] == '\r')) { adv++; }
    stash_consume(adv);

    s_text_lines++;
    handle_text_line(line);
  }
}

void SimHub_GetStats(uint32_t *frames, uint32_t *textLines)
{
  if (frames != NULL) {
    *frames = s_frames_decoded;
  }
  if (textLines != NULL) {
    *textLines = s_text_lines;
  }
}
