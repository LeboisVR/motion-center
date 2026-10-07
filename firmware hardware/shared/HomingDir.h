/* ============================================================================
 * HomingDir.h — direction de homing configurable (librairie commune)
 *
 * Convention produit (protocole Motion Center, toutes les box) :
 *   - SET HOMING_DIR <0|1> <motor>   0 = butee MIN (defaut), 1 = butee MAX
 *   - STATUS : 5e champ par moteur   Mx=conn,cal,max,margin,hdir
 *
 * Semantique de la position posee en fin de homing (apres le recul de
 * securite "margin" hors de la butee) :
 *   - homing MIN : position logique 0 (zero pose a margin de la butee MIN) ;
 *   - homing MAX : position logique = course utile = max - 2*margin
 *     (le haut de la plage mappee par PosMap_TargetOptional, de sorte que la
 *     consigne pleine echelle corresponde exactement a la position posee).
 *
 * Garde-fou : le homing MAX exige un max calibre (max != 0), sinon la
 * position a poser est inconnue -> les FSM retombent sur un homing MIN.
 *
 * Header-only, C pur. Consommateurs : AVR (MinCalibration.cpp), F103/G4
 * (shared Homing.c, mc_protocol.c).
 * ==========================================================================*/
#ifndef HOMINGDIR_H
#define HOMINGDIR_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HOMING_DIR_MIN 0u
#define HOMING_DIR_MAX 1u

/* Le homing MAX n'est permis que si la course est calibree. */
static inline bool HomingDir_MaxAllowed(uint32_t maxPos)
{
  return maxPos != 0u;
}

/* Direction effective d'un homing : MAX demande mais max non calibre ->
   repli MIN (position a poser inconnue sinon). */
static inline bool HomingDir_EffectiveToMax(bool wantMax, uint32_t maxPos)
{
  return wantMax && HomingDir_MaxAllowed(maxPos);
}

/* Position logique posee en fin de homing (apres recul "margin").
   MIN -> 0 ; MAX -> max - 2*margin (clamp 0). */
static inline uint16_t HomingDir_RestPos(bool toMax, uint16_t maxPos,
                                         uint16_t margin)
{
  if (!toMax) { return 0u; }
  uint32_t twice = (uint32_t)margin * 2u;
  return ((uint32_t)maxPos > twice) ? (uint16_t)((uint32_t)maxPos - twice) : 0u;
}

#ifdef __cplusplus
}
#endif

#endif /* HOMINGDIR_H */
