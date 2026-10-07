/*
 * CalibrationUtils.cpp  —  lite / low-RAM/Flash version
 *
 *    v1.1
 *
 * - No Arduino String (uses fixed buffers)
 * - No sprintf/snprintf (smaller flash)
 * - detectMin/detectMax do NOT disable servo (only fullCalibration does)
 */

#include "CalibrationUtils.h"
#include "MotorControl.h"
#include "HardwareAbstraction.h"
#include "CommandParser.h"
#include "MinCalibration.h"
#include "Globals.h"
#include <Arduino.h>
static void logU16_P(const __FlashStringHelper* head, uint16_t v);
// --- Helpers pour le log de calibration vers le GUI -------------------------
static void calLogSimple(uint8_t motor, const __FlashStringHelper* msg) {
  hwLog(F("[CAL] M"));
  logU16_P(F(""), motor);
  hwLog(F(" — "));
  hwLog(msg);
}

static void calLogStep(uint8_t step, uint8_t total, uint8_t motor, const __FlashStringHelper* label) {
  hwLog(F("[CAL] Step "));
  logU16_P(F(""), step);
  hwLog(F("/"));
  logU16_P(F(""), total);
  hwLog(F(" — M"));
  logU16_P(F(""), motor);
  hwLog(F(": "));
  hwLog(label);
}


#define blank_us 200 //wait before each endstop read. The lesser the faster
// ----------------------------------------------------------------------------
// Small helpers (no String / no printf)
// ----------------------------------------------------------------------------
bool timeout = false;
// Bornes "effectives" tenant compte de la marge
static inline uint16_t effMin(uint8_t motor) {
  const uint8_t i = motor - 1;
  return margin[i]; // min logique = marge
}

static inline uint16_t effMax(uint8_t motor) {
  const uint8_t i = motor - 1;
  const uint16_t mx = max[i];
  const uint16_t mar = margin[i];
  // Sécurité : si marge incohérente, on évite les underflow
  if (mx <= mar) return mx;                    // impossible de placer effMax
  if (mx < (uint16_t)(2U * mar)) return mx-1;  // marge trop grande -> borne prudente
  return (uint16_t)(mx - mar);                 // course utile = [mar .. max-mar]
}

static bool readLine(char* dst, uint8_t cap) {
  // Reads a line terminated by \r or \n, returns true when got something.
  uint8_t len = 0;
  while (true) {
    while (Serial.available()) {
      char c = (char)Serial.read();
      if (c == '\r' || c == '\n') {
        if (len > 0) { dst[len] = 0; return true; }
      } else if (len + 1 < cap) {
        dst[len++] = c;
      }
    }
    // avoid busy loop
    hwDelayMs(10);
  }
}

static bool parseU16(const char* s, uint16_t* out) {
  uint32_t v = 0; bool any=false;
  while (*s == ' ' || *s == '\t') ++s;
  while (*s >= '0' && *s <= '9') { v = v*10 + (uint32_t)(*s - '0'); any=true; ++s; if (v > 65535UL) v = 65535UL; }
  if (!any) return false;
  *out = (uint16_t)v; return true;
}

static void logU16_P(const __FlashStringHelper* head, uint16_t v) {
  char b[12]; // up to 65535
  uint16_t x=v; uint8_t i=0, j; char tmp[6];
  if (x==0) { tmp[i++]='0'; }
  else { while (x>0 && i<5) { tmp[i++] = (char)('0' + (x%10)); x/=10; } }
  for (j=0; j<i; ++j) b[j]=tmp[i-1-j];
  b[i]=0;
  hwLog(head);
  hwLog(b);
}
static inline void logLit(const __FlashStringHelper* s) { hwLog(s); }
static inline void logC(const char* s) { hwLog(s); }

static void log2U16(const __FlashStringHelper* h1, uint16_t v1,
                    const __FlashStringHelper* mid,
                    const __FlashStringHelper* h2, uint16_t v2) {
  logLit(h1);        logU16_P(F(""), v1);
  logLit(mid);       // e.g. F(" | ")
  logLit(h2);        logU16_P(F(""), v2);
}

enum ActMotionState : uint8_t { ACT_MOTION_IDLE = 0, ACT_MOTION_COMPRESSING = 1, ACT_MOTION_DECOMPRESSING = 2, ACT_MOTION_CENTERED = 3 };
static uint8_t g_lastActMotion = 255;

static inline void actSetMotion(uint8_t motion) {
  if (motion == g_lastActMotion) return;
  g_lastActMotion = motion;
  if (motion == ACT_MOTION_COMPRESSING) {
    hwLog(F("[ACT] COMPRESSING"));
  } else if (motion == ACT_MOTION_DECOMPRESSING) {
    hwLog(F("[ACT] DECOMPRESSING"));
  } else if (motion == ACT_MOTION_CENTERED) {
    hwLog(F("[ACT] CENTERED"));
  } else {
    hwLog(F("[ACT] IDLE"));
  }
}
static inline void actCompressing()   { actSetMotion(ACT_MOTION_COMPRESSING); }
static inline void actDecompressing() { actSetMotion(ACT_MOTION_DECOMPRESSING); }
static inline void actIdle()          { actSetMotion(ACT_MOTION_IDLE); }
static inline void actCentered()      { actSetMotion(ACT_MOTION_CENTERED); }

// Keep calibration motion in synchronous mode (v1.9-like) to avoid
// ISR/main-loop races on 16-bit motion state during homing loops.

struct ScopedCalibrationMotionGuard {
  ScopedCalibrationMotionGuard() {
    enterCalibrationMotionMode();
  }

  ~ScopedCalibrationMotionGuard() {
    exitCalibrationMotionMode();
  }
};

// Blocking calibration loops run in synchronous mode; speed them up a bit
// versus the configured homing cadence to preserve practical runtime.
static inline uint32_t blockingCalibIntervalUs() {
  uint32_t us = getMinCalibStepIntervalUs();
  if (us < 120U) us = 120U;     // keep a conservative lower bound
  return us;
}

// Lightweight abort poll for tight loops.
// Accepts either:
//  - single-char '!'
//  - short command line starting with "DO CANCEL"
static bool fastAbortPoll() {
  if (calibAbortCheck()) return true;
  if (!Serial.available()) return false;

  char b[24];
  uint8_t n = 0;
  while (Serial.available() && n < (uint8_t)(sizeof(b) - 1)) {
    char c = (char)Serial.read();
    if (c == '!') {
      calibAbortRequest();
      return true;
    }
    if (c == '\r' || c == '\n') break;
    b[n++] = c;
  }
  b[n] = 0;

  char* s = b;
  while (*s == ' ' || *s == '\t') ++s;

  // detect "DO CANCEL" (case-insensitive, lightweight)
  if ((s[0] == 'D' || s[0] == 'd') && (s[1] == 'O' || s[1] == 'o')) {
    char* p = s + 2;
    while (*p == ' ' || *p == '\t') ++p;
    if ((p[0] == 'C' || p[0] == 'c') &&
        (p[1] == 'A' || p[1] == 'a') &&
        (p[2] == 'N' || p[2] == 'n') &&
        (p[3] == 'C' || p[3] == 'c') &&
        (p[4] == 'E' || p[4] == 'e') &&
        (p[5] == 'L' || p[5] == 'l')) {
      calibAbortRequest();
      return true;
    }
  }

  return calibAbortCheck();
}
// ----------------------------------------------------------------------------
// Globals (via Globals.h — mPosition/mTarget routent vers ax[i].pos / ax[i].target)
// ----------------------------------------------------------------------------


void loadCalibration(uint8_t motor) {
  if (motor < 1 || motor > MAX_ACTUATORS) return;

  static const uint16_t maxAddr[] = {
    EEPROM_M1MAX_ADDR, EEPROM_M2MAX_ADDR, EEPROM_M3MAX_ADDR,
    EEPROM_M4MAX_ADDR, EEPROM_M5MAX_ADDR, EEPROM_M6MAX_ADDR
  };
  static const uint16_t marginAddr[] = {
    EEPROM_M1MARGIN_ADDR, EEPROM_M2MARGIN_ADDR, EEPROM_M3MARGIN_ADDR,
    EEPROM_M4MARGIN_ADDR, EEPROM_M5MARGIN_ADDR, EEPROM_M6MARGIN_ADDR
  };

  const uint8_t idx = motor - 1;
  uint16_t maxVal          = hwEepromReadU16(maxAddr[idx]);
  const uint16_t marginVal = hwEepromReadU16(marginAddr[idx]);

  // Legacy 16-bit EEPROM (schema <= 2) : max M1..M4 pouvait valoir 65535.
  // Le bump de EEPROM_SCHEMA_VERSION force un factory reset, ceinture ici.
  if (idx < 4 && maxVal > POSMAP_INPUT_MAX) maxVal = POSMAP_INPUT_MAX;

  if (maxVal == 0) {
    hwLog(F("EEPROM invalid or max hasn't been calibrated yet (=0)"));
    max[idx]        = 0;
    margin[idx]     = 0;
    calibrated[idx] = false;
    return;
  }

  max[idx]        = maxVal;
  margin[idx]     = marginVal;
  calibrated[idx] = true;

  logLit(F("Calibration loaded for M")); logU16_P(F(""), motor);
  logLit(F(" | Max="));                  logU16_P(F(""), maxVal);
  logLit(F(" | Margin="));               logU16_P(F(""), marginVal);
}



void saveCalibration(uint8_t motor) {
  if (motor < 5 || motor > 6) return;  // only M5/M6

  static const uint16_t maxAddr[]    = { EEPROM_M5MAX_ADDR,    EEPROM_M6MAX_ADDR    };
  static const uint16_t marginAddr[] = { EEPROM_M5MARGIN_ADDR, EEPROM_M6MARGIN_ADDR };

  uint8_t eIdx = motor - 5;    // 0->M5, 1->M6
  uint8_t arrIdx = motor - 1;  // 4->M5, 5->M6

  hwEepromWriteU16(maxAddr[eIdx],    max[arrIdx]);
  hwEepromWriteU16(marginAddr[eIdx], margin[arrIdx]);
}


static void waitForStart() {
  hwLog(F("Send 's' to start calibration"));
  unsigned long last = millis();
  for (;;) {
    if (Serial.available()) {
      char c = (char)Serial.read();
      if (c == 's' || c == 'S') { hwLog(F("Starting calibration")); return; }
    }
    if (millis() - last > 3000) { hwLog(F("Send 's' to start")); last = millis(); }
    hwDelayMs(20);
  }
}

void prepareCalibration(uint8_t motor) {
  loadCalibration(motor);
  updateActiveActuatorCount();
  hwLog(F("Calibration sequence. Ensure the actuator can travel between endstops."));
  waitForStart();
  hwLedSet("orange");
}


void moveToMax(uint8_t motor) {
  if (motor < 1 || motor > MAX_ACTUATORS) return;
  ScopedCalibrationMotionGuard motionGuard;
  const uint8_t i = motor - 1;

  hwLog(F("Finding max endstop"));
  enableServo();

  const uint32_t interval_us = blockingCalibIntervalUs();
  const uint8_t  step_size   = getMinCalibStepSize();
  unsigned long  next_step_us = micros();

  const unsigned long TIMEOUT_MS = 50000UL;
  const unsigned long t0 = millis();

  // Approche MAX : avancer jusqu’à déclenchement de l’endstop
  while (!hwReadEndstop(motor)) {
    if ((long)(micros() - next_step_us) >= 0) {
      next_step_us += interval_us;
      uint16_t pos = mPosition[i];
      uint32_t tgt = (uint32_t)pos + step_size;
      if (tgt > 65535U) tgt = 65535U;
      mTarget[i] = (uint16_t)tgt;
    }
    moveMotor();

    if ((millis() - t0) > TIMEOUT_MS || mPosition[i] >= 65530U) {
      hwLog(F("Error: max endstop not reached; aborting"));
      hwLedSet("orange");
      return; // pas de boucle infinie
    }
  }

  // Libère le contact : recule un peu à la même cadence
  unsigned long release_t0 = millis();
  while (hwReadEndstop(motor)) {
    if ((long)(micros() - next_step_us) >= 0) {
      next_step_us += interval_us;
      uint16_t pos = mPosition[i];
      uint16_t dec = step_size;
      mTarget[i] = (uint16_t)(pos - (pos >= dec ? dec : 1));
    }
    moveMotor();

    if ((millis() - release_t0) > 5000UL) break; // filet sécu 5s
  }

  // Enregistre le MAX mécanique (après libération du switch)
  uint16_t maxVal = mPosition[i];
  hwLog(F("Max position recorded: ")); logU16_P(F(""), maxVal);
  max[i] = maxVal;
}
void askAndConfirmMargin(uint8_t motor) {
  const uint8_t idx = motor - 1;

  uint16_t &marginRef = margin[idx];
  volatile uint16_t &posRef = mPosition[idx];
  const uint16_t maxVal = max[idx];

  char line[20];

  for (;;) {
    if (calibAbortCheck()) {
      hwLog(F("[CAL] Aborted in margin selection"));
      // restore any temporary state implicitly by returning
      return;
    }
    hwLog(F("Enter margin in steps (e.g. 100)"));
    if (!readLine(line, sizeof(line))) {
      if (calibAbortCheck()) { hwLog(F("[CAL] Aborted")); return; }
      continue;
    }

    uint16_t m;
    if (!parseU16(line, &m) || m == 0 || (uint32_t)m * 2 >= maxVal) {
      hwLog(F("Invalid margin. Must be >0 and < half stroke."));
      continue;
    }

    const unsigned  originalMargin = marginRef;
    const uint16_t  originalPos    = posRef;

    marginRef = m;

    const uint16_t emi = effMin(motor);
    const uint16_t ema = effMax(motor);
    logLit(F("Margin=")); logU16_P(F(""), m);
    logLit(F(" applied temporarily. Effective travel: ["));
    logU16_P(F(""), emi); logLit(F("..")); logU16_P(F(""), ema);
    logLit(F("] (width ")); logU16_P(F(""), (uint16_t)(ema - emi)); logLit(F(")"));

    testFullStroke(motor);
    if (calibAbortCheck()) { hwLog(F("[CAL] Aborted during margin test")); return; }

    hwLog(F("Confirm margin? (y/n)"));
    if (!readLine(line, sizeof(line))) continue;
    if (line[0]=='y' || line[0]=='Y') {
      hwLog(F("Margin confirmed"));
      return;
    }

    marginRef = originalMargin;
    posRef    = originalPos;
    hwLog(F("Retry margin selection"));
  }
}



#ifndef GUARD_MIN_STEPS
#define GUARD_MIN_STEPS 50   // nombre minimal de pas avant d'autoriser une détection
#endif

bool detectMin(uint8_t motor) {
  if (motor < 5 || motor > 6) { hwLog(F("detectMin supported on M5/M6 only")); return false; }
  ScopedCalibrationMotionGuard motionGuard;
  const uint8_t i = motor - 1;

  // Clear any previous abort request for a fresh run and enable servo
  calibAbortClear();
  enableServo();
  hwLedSet("orange");
  hwLog(F("Detecting MIN using endstop...50s before timeout. If endstop is always on, increase pn024. If too slow, increase PN98 or adjust homing speed in parameters."));
  actCompressing();

  const uint32_t interval_us = blockingCalibIntervalUs();
  const uint8_t  step_size   = getMinCalibStepSize();
  unsigned long  next_step_us = micros();

  // Si déjà sur la butée -> décompresser un peu
  if (hwReadEndstop(motor)) {
    hwLog(F("Endstop already active. Decompressing a bit"));
    actDecompressing();
    uint16_t remain = 50;
    while (remain > 0) {
      if (fastAbortPoll()) {
        hwLog(F("[CAL] Aborted"));
        actIdle();
        disableServo();
        hwLedSet("orange");
        return false;
      }
      if ((long)(micros() - next_step_us) >= 0) {
        next_step_us += interval_us;
        uint16_t pos = mPosition[i];
        uint16_t inc = (remain >= step_size) ? step_size : remain;
        uint32_t tgt = (uint32_t)pos + inc;          // s'éloigner du MIN
        if (tgt > 65535U) tgt = 65535U;
        mTarget[i] = (uint16_t)tgt;
        remain -= inc;
      }
      moveMotor();
    }
  }

  // Démarre en haut pour décrémenter sans underflow
  mPosition[i] = 65535;
  mTarget[i]   = 65535;

  const unsigned long TIMEOUT_MS = 50000UL;
  const unsigned long t0 = millis();

  uint16_t moved = 0; // distance parcourue (en "pas") avant d'autoriser la détection

  // Descente vers MIN
  actCompressing();
  while (true) {
    if (fastAbortPoll()) {
      hwLog(F("[CAL] Aborted"));
      actIdle();
      disableServo();
      hwLedSet("orange");
      return false;
    }
    if ((long)(micros() - next_step_us) >= 0) {
      next_step_us += interval_us;
      uint16_t pos = mPosition[i];
      uint16_t dec = (pos >= step_size) ? step_size : 1;
      mTarget[i] = (uint16_t)(pos - dec);
      moved += step_size;
    }

    moveMotor();
    if (fastAbortPoll()) {
      hwLog(F("[CAL] Aborted"));
      actIdle();
      disableServo();
      hwLedSet("orange");
      return false;
    }

    // Lecture "stable" APRES le mouvement + garde de distance
    bool tripped = false;
    if (moved >= GUARD_MIN_STEPS) {
      tripped = hwReadEndstopStable(motor, blank_us, /*samples*/5, /*need_on*/3);
    }

    if (tripped) break;

    unsigned long now = millis();

    if (now - t0 > TIMEOUT_MS) {
      hwLog(F("Error: detectMin timeout. Increase PN98 or adjust homing speed"));
      hwLog(F("Returning to main menu."));
      actIdle();
      hwLedSet("orange");
      disableServo();
      return false;
    }
  }

  // Backoff: se décoller de la butée (lecture stable)
  actDecompressing();
  uint16_t backoff = (margin[i] ? margin[i] : 20);
  unsigned long released_t0 = millis();

  while (hwReadEndstopStable(motor, blank_us, 5, 3)) {
    if (fastAbortPoll()) {
      hwLog(F("[CAL] Aborted"));
      actIdle();
      disableServo();
      hwLedSet("orange");
      return false;
    }
    if ((long)(micros() - next_step_us) >= 0) {
      next_step_us += interval_us;
      uint16_t pos = mPosition[i];
      uint16_t inc = step_size;
      uint32_t tgt = (uint32_t)pos + inc;   // s'éloigner du switch
      if (tgt > 65535U) tgt = 65535U;
      mTarget[i] = (uint16_t)tgt;
    }
    moveMotor();
    if ((millis() - released_t0) > 5000UL) break; // filet sécu 5s
  }

  // Backoff additionnel logique (marge), même cadence
  while (backoff > 0) {
    if (fastAbortPoll()) {
      hwLog(F("[CAL] Aborted"));
      actIdle();
      disableServo();
      hwLedSet("orange");
      return false;
    }
    if ((long)(micros() - next_step_us) >= 0) {
      next_step_us += interval_us;
      uint16_t pos = mPosition[i];
      uint16_t inc = (backoff >= step_size) ? step_size : backoff;
      uint32_t tgt = (uint32_t)pos + inc;
      if (tgt > 65535U) tgt = 65535U;
      mTarget[i] = (uint16_t)tgt;
      backoff -= inc;
    }
    moveMotor();
  }

  // Zéro logique une fois le MIN trouvé/libéré
  mPosition[i] = 0;
  mTarget[i]   = 0;
  calibrated[motor-1] = true;
  hwLedSet("green");
  hwLog(F("Min endstop found; position set to 0"));
  actIdle();
  return true;
}

#include "MinCalibration.h" // pour getMinCalibStepIntervalUs/Size()

static inline uint16_t effMax(uint8_t motor); // déjà présent

#ifndef GUARD_MAX_STEPS
#define GUARD_MAX_STEPS 50   // nb min de pas avant d'autoriser la détection MAX
#endif

#ifndef MAX_EXTRA_RETRACT_STEPS
#define MAX_EXTRA_RETRACT_STEPS 20  // petit retrait additionnel après libération de la butée MAX
#endif

bool detectMax(uint8_t motor) {
  if (motor < 5 || motor > 6) { hwLog(F("detectMax supported on M5/M6 only")); return false; }
  ScopedCalibrationMotionGuard motionGuard;
  const uint8_t i = motor - 1;

  enableServo();
  hwLedSet("orange");
  hwLog(F("Detecting MAX using endstop.50s before timeout."));
  actDecompressing();

  const uint32_t interval_us = blockingCalibIntervalUs();
  const uint8_t  step_size   = getMinCalibStepSize();
  unsigned long  next_step_us = micros();

  // Si déjà sur la butée -> décompresser un peu (lecture stable)
  if (hwReadEndstop(motor)) {
    hwLog(F("Endstop already on. Decompressing"));
    actDecompressing();
    uint16_t remain = 50;
    while (remain > 0) {
      if (Serial.available()) {
        int p = Serial.peek();
        if (p == '!') { Serial.read(); calibAbortRequest(); hwLog(F("[CAL] Abort requested (quick)")); actIdle(); disableServo(); hwLedSet("orange"); return false; }
      }
      if ((long)(micros() - next_step_us) >= 0) {
        next_step_us += interval_us;
        uint16_t pos = mPosition[i];
        uint16_t dec = (remain >= step_size) ? step_size : remain;
        mTarget[i] = (uint16_t)(pos - (pos >= dec ? dec : 1)); // s’éloigner de la butée
        remain -= dec;
      }
      moveMotor();
    }
  }

  // Démarre bas et monte vers le MAX
  mPosition[i] = 0;
  mTarget[i]   = 0;

  const unsigned long TIMEOUT_MS = 50000UL;
  const unsigned long t0 = millis();

  uint16_t moved = 0; // distance parcourue (en pas) avant d'autoriser la détection

  // Approche MAX : avancer jusqu’à détection stable
  actDecompressing();
  while (true) {
    if (Serial.available()) {
      int p = Serial.peek();
      if (p == '!') { Serial.read(); calibAbortRequest(); hwLog(F("[CAL] Abort requested (quick)")); actIdle(); disableServo(); hwLedSet("orange"); return false; }
    }
    if ((long)(micros() - next_step_us) >= 0) {
      next_step_us += interval_us;
      uint16_t pos = mPosition[i];
      uint32_t tgt = (uint32_t)pos + step_size;
      if (tgt > 65535U) tgt = 65535U;
      mTarget[i] = (uint16_t)tgt;
      moved += step_size;
    }
    moveMotor();

    // Lecture stable APRES mouvement + garde distance
    bool tripped = false;
    if (moved >= GUARD_MAX_STEPS) {
      tripped = hwReadEndstopStable(motor, blank_us, /*samples*/5, /*need_on*/3);
    }
    if (tripped) break;

    if ((millis() - t0) > TIMEOUT_MS) {
      hwLog(F("Error: detectMax timeout. Increase PN98 or adjust homing speed"));
      hwLog(F("Returning to main menu."));
      actIdle();
      hwLedSet("orange");
      disableServo();
      return false;
    }
  }

  // Keep the MAX reference at first stable contact point.
  // Margin is applied later by effMax(), so we must not subtract it here.
  const uint16_t maxContact = mPosition[i];

  // Backoff unique : se décoller de la butée (lecture stable), sans marge additionnelle
  actCompressing();
  unsigned long released_t0 = millis();

  // d’abord libérer le contact, tant que l’endstop reste stablement ON
  while (hwReadEndstopStable(motor, 200, 5, 3)) {
    if (Serial.available()) {
      int p = Serial.peek();
      if (p == '!') { Serial.read(); calibAbortRequest(); hwLog(F("[CAL] Abort requested (quick)")); actIdle(); disableServo(); hwLedSet("orange"); return false; }
    }
    if (calibAbortCheck()) {
      hwLog(F("[CAL] Aborted"));
      actIdle();
      disableServo();
      hwLedSet("orange");
      return false;
    }
    if ((long)(micros() - next_step_us) >= 0) {
      next_step_us += interval_us;
      uint16_t pos = mPosition[i];
      uint16_t dec = step_size;
      mTarget[i]   = (uint16_t)(pos - (pos >= dec ? dec : 1));
    }
    moveMotor();
    if ((millis() - released_t0) > 5000UL) break; // filet sécu
  }

  // Petit retrait additionnel (symétrique au comportement perçu sur detectMin),
  // sans utiliser la marge logicielle.
  uint16_t extraRetract = MAX_EXTRA_RETRACT_STEPS;
  while (extraRetract > 0 && mPosition[i] > 0) {
    if (Serial.available()) {
      int p = Serial.peek();
      if (p == '!') { Serial.read(); calibAbortRequest(); hwLog(F("[CAL] Abort requested (quick)")); actIdle(); disableServo(); hwLedSet("orange"); return false; }
    }
    if (calibAbortCheck()) {
      hwLog(F("[CAL] Aborted"));
      actIdle();
      disableServo();
      hwLedSet("orange");
      return false;
    }
    if ((long)(micros() - next_step_us) >= 0) {
      next_step_us += interval_us;
      uint16_t pos = mPosition[i];
      uint16_t dec = (extraRetract >= step_size) ? step_size : extraRetract;
      mTarget[i]   = (uint16_t)(pos - (pos >= dec ? dec : 1));
      extraRetract -= dec;
    }
    moveMotor();
  }

  // MAX mécanique mesuré au point de contact (avant release)
  const uint16_t rawMax = maxContact;

  // Keep logical position aligned with real physical position after the
  // release/retract phase to avoid introducing a virtual offset.
  const uint16_t currentPos = mPosition[i];
  mTarget[i] = currentPos;

  // Persist measured mechanical MAX from contact point (not from currentPos).
  max[i] = rawMax;
  calibrated[i] = (rawMax != 0);

  hwLedSet("green");
  hwLog(F("Max endstop found: contact/release @ ")); 
  // log “simple”: tu peux utiliser ton logU16_P si dispo
  {
    char b[6];
    // petit helper local si tu ne veux pas inclure logU16_P ici
    uint16_t x = rawMax; char tmp[5]; uint8_t k=0, j=0;
    if(x==0){ b[0]='0'; b[1]=0; }
    else { while(x && k<5){ tmp[k++]=(char)('0'+(x%10)); x/=10; } while(k){ b[j++]=tmp[--k]; } b[j]=0; }
    hwLog(b);
  }
  actIdle();
  return true;
}

// Python-friendly non-blocking endstop queries.
// Return instantaneous endstop state for motor 5 or 6 (true if endstop ON).
bool detectMinPy(uint8_t motor) {
  if (motor < 5 || motor > 6) { hwLog(F("detectMinPy supported on M5/M6 only")); return false; }
  // Use a quick stable-read that adapts its blanking to the configured
  // homing cadence. This avoids false positives when the actuator is
  // moving fast while remaining cheap and fast for Python polling.
  const uint32_t interval_us = blockingCalibIntervalUs();
  // Choose a blanking delay proportional to step cadence but bounded
  // to reasonable values (50..2000 us). Using half the step interval
  // gives a good tradeoff between robustness and latency.
  uint32_t blank = (interval_us > 0) ? (interval_us / 2U) : 200U;
  if (blank < 50U) blank = 50U;
  if (blank > 2000U) blank = 2000U;
  // Use a small sample set for speed (3 samples, need 1 ON to be considered tripped)
  return hwReadEndstopStable(motor, (uint16_t)blank, /*samples*/3, /*need_on*/1);
}

bool detectMaxPy(uint8_t motor) {
  if (motor < 5 || motor > 6) { hwLog(F("detectMaxPy supported on M5/M6 only")); return false; }
  // Mirror detectMinPy: adapt blanking to homing cadence to avoid
  // spurious readings when stepping at higher speeds.
  const uint32_t interval_us = blockingCalibIntervalUs();
  uint32_t blank = (interval_us > 0) ? (interval_us / 2U) : 200U;
  if (blank < 50U) blank = 50U;
  if (blank > 2000U) blank = 2000U;
  return hwReadEndstopStable(motor, (uint16_t)blank, /*samples*/3, /*need_on*/1);
}
void testFullStroke(uint8_t motor) {
  ScopedCalibrationMotionGuard motionGuard;
  const uint8_t  i       = motor - 1;

  // Make TEST_STROKE robust from any starting point by re-referencing MIN.
  // Without this, mPosition can be stale and the first move may miss physical stop.
  calLogSimple(motor, F("Reference MIN before TEST_STROKE"));
  if (!detectMin(motor)) {
    calLogSimple(motor, F("ABORTED during TEST_STROKE"));
    actIdle();
    return;
  }

  const uint16_t minEff  = effMin(motor);
  const uint16_t maxEff  = effMax(motor);

  // Paramètres de vitesse de test déjà existants
  const uint32_t interval_us = blockingCalibIntervalUs();
  const uint8_t  step_size   = getMinCalibStepSize();
  unsigned long  next_step_us = micros();

  calLogSimple(motor, F("Start TEST_STROKE"));
  hwLog(F("Testing full stroke with margin."));
  enableServo();

  // ---- Étape 1/3 : aller à la borne min effective (descente) ---------------
  calLogStep(1, 3, motor, F("Reaching effective MIN"));
  hwLog(F("Reaching effective MIN"));
  actCompressing();
  while (mPosition[i] > minEff) {
    if (Serial.available()) {
      int p = Serial.peek();
      if (p == '!') { Serial.read(); calibAbortRequest(); calLogSimple(motor, F("ABORTED during TEST_STROKE")); actIdle(); disableServo(); hwLedSet("orange"); return; }
    }
    if ((long)(micros() - next_step_us) >= 0) {
      next_step_us += interval_us;
      uint16_t pos = mPosition[i];
      uint16_t dec = step_size;
      uint16_t tgt = (pos > dec) ? (uint16_t)(pos - dec) : minEff;
      if (tgt < minEff) tgt = minEff;      // clamp
      mTarget[i] = tgt;
    }
    moveMotor();
    if (calibAbortCheck()) {
      calLogSimple(motor, F("ABORTED during TEST_STROKE"));
      actIdle();
      disableServo();
      hwLedSet("orange");
      return;
    }
  }

  hwDelayMs(300);

  // ---- Étape 2/3 : aller à la borne max effective (montée) -----------------
  calLogStep(2, 3, motor, F("Reaching effective MAX"));
  hwLog(F("Reaching effective MAX"));
  actDecompressing();
  while (mPosition[i] < maxEff) {
    if (Serial.available()) {
      int p = Serial.peek();
      if (p == '!') { Serial.read(); calibAbortRequest(); calLogSimple(motor, F("ABORTED during TEST_STROKE")); actIdle(); disableServo(); hwLedSet("orange"); return; }
    }
    if ((long)(micros() - next_step_us) >= 0) {
      next_step_us += interval_us;
      uint16_t pos = mPosition[i];
      uint32_t t   = (uint32_t)pos + (uint32_t)step_size;
      if (t > maxEff) t = maxEff;          // clamp
      mTarget[i] = (uint16_t)t;
    }
    moveMotor();
    if (calibAbortCheck()) {
      calLogSimple(motor, F("ABORTED during TEST_STROKE"));
      actIdle();
      disableServo();
      hwLedSet("orange");
      return;
    }
  }

  hwDelayMs(300);

  // ---- Étape 3/3 : retour au centre utile ----------------------------------
  const uint16_t center = (uint16_t)((minEff + maxEff) / 2U);
  calLogStep(3, 3, motor, F("Reaching CENTER"));
  hwLog(F("Reaching CENTER"));

  actCompressing();
  while (mPosition[i] > center) {
    if (Serial.available()) {
      int p = Serial.peek();
      if (p == '!') { Serial.read(); calibAbortRequest(); calLogSimple(motor, F("ABORTED during TEST_STROKE")); actIdle(); disableServo(); hwLedSet("orange"); return; }
    }
    if ((long)(micros() - next_step_us) >= 0) {
      next_step_us += interval_us;
      uint16_t pos = mPosition[i];
      uint16_t dec = step_size;
      uint16_t tgt = (pos > dec) ? (uint16_t)(pos - dec) : center;
      if (tgt < center) tgt = center;      // clamp
      mTarget[i] = tgt;
    }
    moveMotor();
    if (calibAbortCheck()) {
      calLogSimple(motor, F("ABORTED during TEST_STROKE"));
      actIdle();
      disableServo();
      hwLedSet("orange");
      return;
    }
  }

  actDecompressing();
  while (mPosition[i] < center) {
    if (Serial.available()) {
      int p = Serial.peek();
      if (p == '!') { Serial.read(); calibAbortRequest(); calLogSimple(motor, F("ABORTED during TEST_STROKE")); actIdle(); disableServo(); hwLedSet("orange"); return; }
    }
    if ((long)(micros() - next_step_us) >= 0) {
      next_step_us += interval_us;
      uint16_t pos = mPosition[i];
      uint32_t t   = (uint32_t)pos + (uint32_t)step_size;
      if (t > center) t = center;          // clamp
      mTarget[i] = (uint16_t)t;
    }
    moveMotor();
    if (calibAbortCheck()) {
      calLogSimple(motor, F("ABORTED during TEST_STROKE"));
      actIdle();
      disableServo();
      hwLedSet("orange");
      return;
    }
  }

  calLogSimple(motor, F("TEST_STROKE done"));
  actCentered();
}



void fullCalibration(uint8_t motor) {
  // start fresh: clear abort flag
  calibAbortClear();
  prepareCalibration(motor);
  if (!detectMin(motor)) return;
  hwDelayMs(150);
  if (!detectMax(motor)) return;

  const uint8_t i = motor - 1;
  uint16_t measuredMax = max[i];
  calibrated[i] = (measuredMax != 0);
  homed[motor - 1] = true;
  saveCalibration(motor);

  disableServo();
  hwLedSet("green");
  hwLog(F("Calibration done. Margin can be set manually from Status tab."));
}

// -----------------------------------------------------------------------------
// Soft-only calibration (non-interactive)
// -----------------------------------------------------------------------------
bool fullCalibrationSoft(uint8_t motor) {
  if (motor < 5 || motor > 6) {
    hwLog(F("fullCalibrationSoft supported on M5/M6 only"));
    return false;
  }

  // Clear any previous abort request and inform GUI
  calibAbortClear();
  // Info pour le GUI
  calLogSimple(motor, F("Start COMPLETE_CALIB"));

  // Ensure config is loaded (connected flags, homing speed, ...)
  loadConfig();
  updateActiveActuatorCount();

  // --- Étape 1/3 : recherche du MIN -----------------------------------------
  calLogStep(1, 3, motor, F("Detecting MIN endstop"));
  if (!detectMin(motor)) {
    calLogSimple(motor, F("FAILED during MIN detection"));
    return false;
  }

  hwDelayMs(150);

  // --- Étape 2/3 : recherche du MAX -----------------------------------------
  calLogStep(2, 3, motor, F("Detecting MAX endstop"));
  if (!detectMax(motor)) {
    calLogSimple(motor, F("FAILED during MAX detection"));
    return false;
  }

  if (calibAbortCheck()) {
    calLogSimple(motor, F("ABORTED"));
    return false;
  }

  // max[] has already been set by detectMax() from contact point.
  const uint8_t i = motor - 1;
  calibrated[i] = (max[i] != 0);
  homed[motor - 1] = true;

  // --- Étape 3/3 : sauvegarde / fin -----------------------------------------
  calLogStep(3, 3, motor, F("Saving calibration to EEPROM"));
  // Persist (uses current margin[] value)
  saveCalibration(motor);

  disableServo();
  hwLedSet("green");
  // Texte déjà utilisé par la v1.7 / v1.8
  hwLog(F("Calibration done. Please restart the control box before using Simhub"));

  calLogSimple(motor, F("COMPLETE_CALIB done"));
  return true;
}

bool goToCenterSmart(uint8_t motor) {
  if (motor < 5 || motor > 6) {
    hwLog(F("goToCenterSmart supported on M5/M6 only"));
    return false;
  }

  const uint8_t i = motor - 1;
  calibAbortClear();

  // If we don't know current position, re-reference on MIN first.
  if (!homed[i]) {
    calLogSimple(motor, F("Position unknown -> detect MIN before centering"));
    if (!detectMin(motor)) {
      calLogSimple(motor, F("FAILED during MIN detection"));
      return false;
    }
  }

  if (max[i] == 0) {
    hwLog(F("Cannot center: MAX not calibrated"));
    return false;
  }

  const uint16_t center = (uint16_t)((effMin(motor) + effMax(motor)) / 2U);
  const uint32_t interval_us = blockingCalibIntervalUs();
  const uint8_t step_size = getMinCalibStepSize();
  unsigned long next_step_us = micros();

  enableServo();
  hwLedSet("orange");

  while (mPosition[i] != center) {
    if (fastAbortPoll()) {
      calLogSimple(motor, F("ABORTED"));
      actIdle();
      disableServo();
      hwLedSet("orange");
      return false;
    }

    if ((long)(micros() - next_step_us) >= 0) {
      next_step_us += interval_us;
      const uint16_t pos = mPosition[i];

      if (pos > center) {
        actCompressing();
        uint16_t dec = step_size;
        uint16_t tgt = (pos > dec) ? (uint16_t)(pos - dec) : center;
        if (tgt < center) tgt = center;
        mTarget[i] = tgt;
      } else {
        actDecompressing();
        uint32_t tgt = (uint32_t)pos + (uint32_t)step_size;
        if (tgt > center) tgt = center;
        mTarget[i] = (uint16_t)tgt;
      }
    }

    moveMotor();
  }

  mTarget[i] = center;
  homed[i] = true;
  actCentered();
  hwLedSet("green");
  calLogSimple(motor, F("GO_TO_CENTER done"));
  return true;
}

// ---------------------------------------------------------------------------
// Endpark (SH_DISABLE) : rejoint doucement endParkPct% de la course utile
// (max - 2*margin) sur M5/M6 avant la coupure servo.
//
// NON BLOQUANT (begin/tick) : un park bloquant gelait loop() pendant des
// secondes ; fastAbortPoll() avalait alors toute commande recue (ex. SET
// ENDPARK jamais persiste) et le buffer RX debordait (Write timeout cote
// app). Ici loop() continue de servir le protocole, l'ISR Timer3 emet les
// pas, et la fin (servo off + reponse SH_DISABLED) est differee via
// shDisableFinalize().
// ---------------------------------------------------------------------------
extern bool servoEnabled;   // défini dans MotorControl.cpp

static bool          s_epActive = false;
static bool          s_epEn[2];
static uint16_t      s_epTgt[2];
static uint32_t      s_epIntervalUs;
static uint8_t       s_epStep;
static unsigned long s_epDeadlineMs;
static unsigned long s_epNextStepUs;

bool endParkActive() { return s_epActive; }

void endParkCancel() { s_epActive = false; }

bool endParkBegin() {
  if (s_epActive) return true;
  if (!servoEnabled) return false;

  uint16_t maxDelta = 0;
  s_epEn[0] = s_epEn[1] = false;

  for (uint8_t k = 0; k < 2; k++) {
    const uint8_t i = (uint8_t)(4 + k);
    if (i >= MAX_ACTUATORS) break;
    const uint8_t pct = axCfg[i].endParkPct;
    if (pct > 100) continue;                       // off
    if (!mConnected[i] || !homed[i] || max[i] == 0) continue;
    const uint16_t course = (max[i] > (uint16_t)(2U * margin[i]))
                              ? (uint16_t)(max[i] - 2U * margin[i]) : 0U;
    s_epTgt[k] = (uint16_t)(((uint32_t)course * pct) / 100UL);
    s_epEn[k]  = true;
    const uint16_t pos = atomicGetPosition(i);
    const uint16_t d = (pos > s_epTgt[k]) ? (uint16_t)(pos - s_epTgt[k])
                                          : (uint16_t)(s_epTgt[k] - pos);
    if (d > maxDelta) maxDelta = d;
  }
  if (!s_epEn[0] && !s_epEn[1]) return false;

  hwLog(F("[PARK] Endpark start"));
  hwLedSet("orange");
  calibAbortClear();

  s_epIntervalUs = blockingCalibIntervalUs();
  s_epStep       = getMinCalibStepSize();
  const uint16_t sps = getMinCalibStepsPerSecond();
  s_epDeadlineMs = millis() + (unsigned long)maxDelta * 1000UL / (sps ? sps : 1U) + 5000UL;
  s_epNextStepUs = micros();
  s_epActive = true;
  return true;
}

void endParkTick() {
  if (!s_epActive) return;

  bool pending = false;
  for (uint8_t k = 0; k < 2; k++) {
    if (s_epEn[k] && atomicGetPosition((uint8_t)(4 + k)) != s_epTgt[k]) {
      pending = true;
      break;
    }
  }

  if (!pending || (long)(millis() - s_epDeadlineMs) >= 0 ||
      calibAbortCheck() || !servoEnabled) {
    s_epActive = false;
    hwLog(F("[PARK] Endpark done"));
    shDisableFinalize();     // procedure d'arret existante + SH_DISABLED
    return;
  }

  if ((long)(micros() - s_epNextStepUs) >= 0) {
    s_epNextStepUs += s_epIntervalUs;
    for (uint8_t k = 0; k < 2; k++) {
      if (!s_epEn[k]) continue;
      const uint8_t  i   = (uint8_t)(4 + k);
      const uint16_t pos = atomicGetPosition(i);
      if (pos == s_epTgt[k]) continue;
      if (pos > s_epTgt[k]) {
        const uint16_t d = (uint16_t)(pos - s_epTgt[k]);
        atomicSetTarget(i, (uint16_t)(pos - ((d > s_epStep) ? s_epStep : d)));
      } else {
        uint32_t nt = (uint32_t)pos + s_epStep;
        if (nt > s_epTgt[k]) nt = s_epTgt[k];
        atomicSetTarget(i, (uint16_t)nt);
      }
    }
  }
}


