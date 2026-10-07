/* ============================================================================
 * PFrame.h — format de la trame cible SimHub 'P' (librairie commune)
 *
 * Profil SimHub unique "Lebois Racing Motion Center" :
 *   'P' + 7 x uint16 big-endian (0..32767, 15 bits), SANS terminateur.
 *   TOUTES les box recoivent 7 axes ; celles qui en gerent moins consomment
 *   la trame complete et ignorent les axes surnumeraires (ne JAMAIS lire
 *   moins de PFRAME_LEN octets : 2 octets orphelins par trame suffisent a
 *   desynchroniser le parseur — cause reelle des saccades F103 de 2026-07).
 *
 * Header-only, C pur. Consommateurs : AVR competition-control-box-v2.1,
 * F103 PRO Control Box, G4 Competition Control Box.
 * ==========================================================================*/
#ifndef PFRAME_H
#define PFRAME_H

#include <stdint.h>
#include "PosMap.h"   /* POSMAP_INPUT_MAX : pleine echelle 15 bits */

#ifdef __cplusplus
extern "C" {
#endif

#define PFRAME_TAG          'P'
#define PFRAME_AXES         7U
#define PFRAME_PAYLOAD_LEN  (2U * PFRAME_AXES)        /* 14 octets            */
#define PFRAME_LEN          (1U + PFRAME_PAYLOAD_LEN) /* 15 octets, tag inclus */

/* Decode l'axe `axis` (0..6) depuis le payload (14 octets apres le tag).
   Retourne la consigne 15 bits (0..32767), bit 15 masque. */
static inline uint16_t PFrame_Axis(const uint8_t *payload, uint8_t axis)
{
  return (uint16_t)((((uint16_t)payload[2U * axis] << 8)
                     | (uint16_t)payload[2U * axis + 1U]) & POSMAP_INPUT_MAX);
}

#ifdef __cplusplus
}
#endif

#endif /* PFRAME_H */
