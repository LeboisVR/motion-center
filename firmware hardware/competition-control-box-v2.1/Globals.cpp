// V1.0 code version

#include "Globals.h"
#include "HardwareAbstraction.h"
#include "PosMap.h"   // librairie commune : POSMAP_INPUT_MAX (32767)

uint16_t margin[MAX_ACTUATORS]    = {0};                            // zero-init all
// Convention 15 bits (profil SimHub 0..32767) : max/margin M1..M4 sont exprimes
// en unites 15 bits. La conversion en pas physiques (x2) se fait au mapping.
unsigned max[MAX_ACTUATORS]       = {POSMAP_INPUT_MAX, POSMAP_INPUT_MAX,
                                     POSMAP_INPUT_MAX, POSMAP_INPUT_MAX};  // M5+ → 0 by default
bool mConnected[MAX_ACTUATORS]    = { true, true, true, true };      // M5+ → false by default
bool calibrated[MAX_ACTUATORS]= {0};

// Instances des shims — la mémoire réelle est dans ax[i].pos / ax[i].target.
PosProxy mPosition;
TgtProxy mTarget;

void FactoryReset(bool m5Connected, bool m6Connected) {

    // Erase entire EEPROM before rewriting so no stale bytes from a previous
    // layout can corrupt the new configuration.
    hwEepromErase();

    // --- Reset RAM defaults first (so runtime behavior matches EEPROM) ---
    for (uint8_t i = 0; i < 4; i++) {         // M1..M4
      max[i] = POSMAP_INPUT_MAX;               // convention 15 bits (profil SimHub)
      margin[i] = 0;
      calibrated[i] = true;
      homed[i] = false;
    }
    for (uint8_t i = 4; i < MAX_ACTUATORS; i++) {  // M5 (and M6 when present)
      max[i] = 0;
      margin[i] = 0;
      calibrated[i] = false;
      homed[i] = false;
    }

    // Connectivity in RAM
    mConnected[0] = true;
    mConnected[1] = true;
    mConnected[2] = true;
    mConnected[3] = true;
    mConnected[4] = m5Connected;
#if MAX_ACTUATORS > 5
    mConnected[5] = m6Connected;
#endif

    // --- Persist to EEPROM ---
    // MAX (M1..M4 defaults)
    hwEepromWriteU16(EEPROM_M1MAX_ADDR,    (uint16_t)max[0]);
    hwEepromWriteU16(EEPROM_M2MAX_ADDR,    (uint16_t)max[1]);
    hwEepromWriteU16(EEPROM_M3MAX_ADDR,    (uint16_t)max[2]);
    hwEepromWriteU16(EEPROM_M4MAX_ADDR,    (uint16_t)max[3]);

    // MARGIN (M1..M4 defaults)
    hwEepromWriteU16(EEPROM_M1MARGIN_ADDR, (uint16_t)margin[0]);
    hwEepromWriteU16(EEPROM_M2MARGIN_ADDR, (uint16_t)margin[1]);
    hwEepromWriteU16(EEPROM_M3MARGIN_ADDR, (uint16_t)margin[2]);
    hwEepromWriteU16(EEPROM_M4MARGIN_ADDR, (uint16_t)margin[3]);

    // Clear calibration values for optional actuators (M5/M6)
    hwEepromWriteU16(EEPROM_M5MAX_ADDR,    0);
    hwEepromWriteU16(EEPROM_M5MARGIN_ADDR, 0);
    hwEepromWriteU16(EEPROM_M6MAX_ADDR,    0);
    hwEepromWriteU16(EEPROM_M6MARGIN_ADDR, 0);

    // Default connectivity: M1–M4 always present
    hwEepromWriteU16(EEPROM_M1CONNECTED_ADDR, 1);
    hwEepromWriteU16(EEPROM_M2CONNECTED_ADDR, 1);
    hwEepromWriteU16(EEPROM_M3CONNECTED_ADDR, 1);
    hwEepromWriteU16(EEPROM_M4CONNECTED_ADDR, 1);

    // Persist connectivity for optional actuators
    hwEepromWriteU16(EEPROM_M5CONNECTED_ADDR, m5Connected ? 1 : 0);
    hwEepromWriteU16(EEPROM_M6CONNECTED_ADDR, m6Connected ? 1 : 0);

    // Homing direction back to factory default (MIN endstop)
    hwEepromWriteU16(EEPROM_M5HOMINGDIR_ADDR, 0);
    hwEepromWriteU16(EEPROM_M6HOMINGDIR_ADDR, 0);
    axCfg[4].hometoMax = false;
#if MAX_ACTUATORS > 5
    axCfg[5].hometoMax = false;
#endif

    // Endpark back to factory default (off = stay in place on SH_DISABLE)
    hwEepromWriteU16(EEPROM_M5ENDPARK_ADDR, ENDPARK_OFF);
    hwEepromWriteU16(EEPROM_M6ENDPARK_ADDR, ENDPARK_OFF);
    axCfg[4].endParkPct = ENDPARK_OFF;
#if MAX_ACTUATORS > 5
    axCfg[5].endParkPct = ENDPARK_OFF;
#endif

    // Marker — stores schema version so a future layout change auto-triggers reset
    hwEepromWriteU16(EEPROM_HAS_BEEN_FACTORYRESET, EEPROM_SCHEMA_VERSION);

    // Recompute expected bytes for P frames (depends on connected)
    recomputeExpectedBytesP();

    hwLog(F("Factory settings restored. Power-cycle is recommended."));
}


uint8_t activeMotorsCount = 4;   // au moins M1..M4
uint8_t expectedBytesP     = 8;  // 2 octets * 4 moteurs
void recomputeExpectedBytesP() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_ACTUATORS; i++) {
    if (mConnected[i]) n++;
  }
  if (n < 4) n = 4;              // compat : les 4 de base
  activeMotorsCount = n;
  // La taille de la frame P est TOUJOURS fixée à MAX_ACTUATORS axes.
  // Le profil SimHub est compilé pour MAX_ACTUATORS (5 en V1, 6 en V2) ;
  // si on réduisait expectedBytesP quand M5/M6 sont déconnectés, le protocole
  // se désynchroniserait. Les axes absents sont simplement ignorés dans la boucle.
  expectedBytesP = (uint8_t)(2 * MAX_ACTUATORS);
}