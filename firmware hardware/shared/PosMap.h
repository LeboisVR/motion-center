/* ============================================================================
 * PosMap.h — mapping consigne SimHub 15 bits -> cible moteur (librairie commune)
 *
 * Convention produit (profil SimHub unique "Lebois Racing Motion Center") :
 *   - trame 'P' + 7x uint16 BE, valeurs 0..32767 (15 bits) ;
 *   - MAX des axes de base M1..M4 = unites 15 bits, defaut usine 32767,
 *     l'utilisateur ne peut pas saisir plus (clamp firmware + Motion Center) ;
 *   - MAX/MARGIN des axes optionnels M5/M6 = pas physiques mesures au homing.
 *
 * Header-only, C pur (utilisable depuis les projets C++/Arduino et C/STM32).
 * Consommateurs : AVR competition-control-box-v2.1, F103 PRO Control Box,
 * G4 Competition Control Box (axes 1:1 sur 32767 pas -> mapping identite).
 * ==========================================================================*/
#ifndef POSMAP_H
#define POSMAP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pleine echelle de la consigne SimHub (15 bits) = MAX usine des axes M1..M4. */
#define POSMAP_INPUT_MAX 32767U

/* Mapping generique u (0..32767) -> [lo..hi] avec arrondi.
   Filet de secu : hi <= lo -> lo (aucune course utile). */
static inline uint16_t PosMap_Map(uint16_t u, uint16_t lo, uint16_t hi)
{
  if (hi <= lo) { return lo; }
  uint32_t span = (uint32_t)hi - (uint32_t)lo;
  uint32_t pos  = ((uint32_t)(u & POSMAP_INPUT_MAX) * span + (POSMAP_INPUT_MAX / 2U))
                  / POSMAP_INPUT_MAX;                    /* arrondi au plus pres */
  uint32_t out  = (uint32_t)lo + pos;
  if (out > 65535UL) { out = 65535UL; }                  /* ceinture/bretelles   */
  return (uint16_t)out;
}

/* Axes de base M1..M4 : maxPos/margin en unites 15 bits (0..32767).
   stepScale = pas physiques par unite 15 bits :
     - 2 sur AVR (course complete ~65535 pas),
     - 1 sur STM32 F103/G4 (course 32767 pas).
   Course utile = [margin .. maxPos - margin] (en unites), convertie en pas. */
static inline uint16_t PosMap_TargetBase(uint16_t u, uint16_t maxPos,
                                         uint16_t margin, uint8_t stepScale)
{
  uint16_t hi15 = (maxPos > margin) ? (uint16_t)(maxPos - margin) : maxPos;
  uint16_t lo   = (uint16_t)((uint32_t)margin * (uint32_t)stepScale);
  uint16_t hi   = (uint16_t)((uint32_t)hi15  * (uint32_t)stepScale);
  return PosMap_Map(u, lo, hi);
}

/* Axes optionnels M5/M6 : maxPos/margin en pas physiques mesures.
   Le zero logique a deja consomme une margin au retrait post-homing :
   course utile = [0 .. maxPos - 2*margin]. */
static inline uint16_t PosMap_TargetOptional(uint16_t u, uint16_t maxPos,
                                             uint16_t margin)
{
  uint32_t twice = (uint32_t)margin * 2UL;
  uint16_t hi = ((uint32_t)maxPos > twice) ? (uint16_t)((uint32_t)maxPos - twice) : 0U;
  return PosMap_Map(u, 0U, hi);
}

#ifdef __cplusplus
}
#endif

#endif /* POSMAP_H */
