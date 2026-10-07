/**
 ******************************************************************************
 * @file    simhub_parser.h
 * @brief   Parser protocole SimHub sur USB CDC (trames cibles + handshake).
 *
 *  - CDC_Receive_FS (contexte IT USB) appelle SimHub_RxPush() : copie dans un
 *    ring buffer et rearme immediatement la reception.
 *  - SimHub_Process() (boucle principale) vide le ring dans un stash, decode
 *    trame par trame en validant le terminateur, et pousse les cibles via
 *    Motion_SetTarget().
 *
 *  Trame cible : 'T' + MOTION_NUM_AXES x uint16 big-endian + '\n'.
 *  Handshake   : "SH_START" -> reponse "CALIBRATED".
 ******************************************************************************
 */
#ifndef SIMHUB_PARSER_H
#define SIMHUB_PARSER_H

#include <stdint.h>
#include "motion_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Initialise le parser (ring + stash vides). */
void SimHub_Init(void);

/**
 * @brief Pousse des octets recus dans le ring (appelee depuis CDC_Receive_FS).
 *        Sure en contexte IT. Les octets en exces (ring plein) sont jetes.
 */
void SimHub_RxPush(const uint8_t *buf, uint32_t len);

/**
 * @brief Vide le ring, decode les trames/commandes, applique les cibles.
 *        A appeler dans la boucle principale (non bloquant).
 */
void SimHub_Process(void);

void SimHub_GetStats(uint32_t *frames, uint32_t *textLines);

#ifdef __cplusplus
}
#endif

#endif /* SIMHUB_PARSER_H */
