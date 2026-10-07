/*
 * CommandParser.cpp (lean, human-only)
 *
 * Code version : 2.0 (API removed, D1/D2… removed, literals in flash via F("...") for hwLog)
 *
 * HUMAN mode:
 *   D        -> summary table (readable)
 *   P        -> parameter wizard (Connected/Max/Margin/Homing SPS)
 *   F        -> function menu (calibration/tests)
 *   !        -> request abort (during long ops)
 */
//#define DEBUG
#include "CommandParser.h"
#include "CalibrationUtils.h"
#include "MotorControl.h"
#include "HardwareAbstraction.h"
#include "MinCalibration.h"
#include "Globals.h"

#include <Arduino.h>
#include <string.h>

// ---- Optional: AVR compatibility for PROGMEM, and safe fallbacks for non-AVR
#if defined(ARDUINO_ARCH_AVR)
  #include <avr/pgmspace.h>
#else
  #ifndef PROGMEM
    #define PROGMEM
  #endif
  #ifndef pgm_read_word
    #define pgm_read_word(p) (*(const uint16_t*)(p))
  #endif
#endif

// ---- Stringify helper for numeric macros (e.g., BOX_VERSION)
#ifndef STR
#define STR(x) #x
#endif
#ifndef XSTR
#define XSTR(x) STR(x)
#endif

// Déclarés ailleurs :
extern void recomputeExpectedBytesP();
extern bool servoEnabled;
extern void updateActiveActuatorCount();

// ======== FORWARD DECLARATIONS ========
static void  handleCommand_API(const char* cmd);
static void  apiPrintStatus();
static void  apiReplyOK();
static void  apiReplyERR(const __FlashStringHelper* msg);
static inline bool quickAbortCheck();

// quick abort helper: peek for '!' and trigger abort actions without consuming other input.
static inline bool quickAbortCheck() {
  if (Serial.available()) {
    int p = Serial.peek();
    if (p == '!') {
      Serial.read();
      calibAbortRequest();
      hwLog(F("[CAL] Abort requested (quick)"));
      disableServo();
      hwLedSet("orange");
      return true;
    }
  }
  return false;
}

// -----------------------------------------------------------------------------
// Input buffering
// -----------------------------------------------------------------------------
static char cmdBuf[48];
static uint8_t cmdLen = 0;
static unsigned long cmdStart = 0;
static const unsigned long CMD_TIMEOUT = 50;

// Backwards-compatible aliases for older identifier variants used elsewhere
#define cmd_buf  cmdBuf
#define cmd_len  cmdLen
#define cmd_start cmdStart

// -----------------------------------------------------------------------------
// Helpers (sans libc lourde)
// -----------------------------------------------------------------------------
static inline char toUpperAZ(char c){ return (c>='a'&&c<='z')?(char)(c-32):c; }

static inline bool streq(const char* a, const char* b){
  while(*a && *b){ if(*a!=*b) return false; ++a; ++b; }
  return *a==0 && *b==0;
}

static void strtoupper_inplace(char* s){
  while(*s){ *s = toUpperAZ(*s); ++s; }
}

// parse entier non signé (base 10), sans atoi/strtoul
static bool parseU32(const char* s, uint32_t* out) {
  uint32_t v = 0; bool any=false;
  while (*s==' '||*s=='\t') ++s;
  while (*s>='0' && *s<='9') { any=true; v = v*10u + (uint32_t)(*s-'0'); ++s; }
  if (!any) return false;
  *out = v; return true;
}

// Parse signed 32-bit integer (base 10), allows leading + or -
static bool parseS32(const char* s, int32_t* out) {
  int32_t sign = 1; int32_t v = 0; bool any=false;
  while (*s==' '||*s=='\t') ++s;
  if (*s == '+' || *s == '-') { if (*s == '-') sign = -1; ++s; }
  while (*s>='0' && *s<='9') { any=true; v = v*10 + (int32_t)(*s - '0'); ++s; }
  if (!any) return false;
  *out = v * sign; return true;
}

// Tokenizer for small in-place command lines.
// Splits on spaces/tabs by writing '\0' delimiters.
// Returns nullptr if there is no more token.
static char* nextTok(char** cursor){
  if(!cursor || !*cursor) return nullptr;
  char* s = *cursor;
  while(*s==' ' || *s=='\t') ++s;
  if(*s==0) { *cursor = s; return nullptr; }
  char* tok = s;
  while(*s && *s!=' ' && *s!='\t') ++s;
  if(*s){ *s = 0; ++s; }
  *cursor = s;
  return tok;
}

// Parse a token into u8 (0..255). Returns false if invalid.
static bool parseU8Tok(const char* tok, uint8_t* out){
  if(!tok || !out) return false;
  uint32_t v=0;
  if(!parseU32(tok, &v)) return false;
  if(v>255UL) return false;
  *out = (uint8_t)v;
  return true;
}

// u16 -> cstr (rapide, sans printf). Retourne la longueur.
static uint8_t u16_to_cstr(uint16_t x, char* out) {
  char tmp[5]; uint8_t i=0, j=0;
  if (x==0) { out[0]='0'; out[1]=0; return 1; }
  while (x && i<5) { tmp[i++] = (char)('0' + (x%10)); x/=10; }
  while (i) out[j++] = tmp[--i];
  out[j]=0; return j;
}
static void logU16_inline(const __FlashStringHelper* head, uint16_t v) {
  char b[6]; u16_to_cstr(v, b); hwLog(head); hwLog(b);
}

// -----------------------------------------------------------------------------
// Soft protocol helpers (machine-readable replies)
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// EEPROM tables
// -----------------------------------------------------------------------------
static const uint16_t maxAddr[] PROGMEM = {
  EEPROM_M1MAX_ADDR, EEPROM_M2MAX_ADDR, EEPROM_M3MAX_ADDR,
  EEPROM_M4MAX_ADDR, EEPROM_M5MAX_ADDR, EEPROM_M6MAX_ADDR
};
static const uint16_t marginAddr[] PROGMEM = {
  EEPROM_M1MARGIN_ADDR, EEPROM_M2MARGIN_ADDR, EEPROM_M3MARGIN_ADDR,
  EEPROM_M4MARGIN_ADDR, EEPROM_M5MARGIN_ADDR, EEPROM_M6MARGIN_ADDR
};
static const uint16_t connectedAddr[] PROGMEM = {
  EEPROM_M1CONNECTED_ADDR, EEPROM_M2CONNECTED_ADDR, EEPROM_M3CONNECTED_ADDR,
  EEPROM_M4CONNECTED_ADDR, EEPROM_M5CONNECTED_ADDR, EEPROM_M6CONNECTED_ADDR
};
static inline uint16_t eeAddr_P(const uint16_t* base, uint8_t idx) {
  return pgm_read_word(&base[idx]);
}

static void apiReplyOK(){
  Serial.println(F("OK"));
}

static uint8_t homingStepSizeFromSps(uint32_t sps) {
  if (sps >= 9000UL) return 16;
  if (sps >= 6000UL) return 12;
  if (sps >= 3500UL) return 8;
  if (sps >= 1800UL) return 5;
  if (sps >= 800UL)  return 3;
  if (sps >= 300UL)  return 2;
  return 1;
}

static void apiReplyERR(const __FlashStringHelper* msg){
  Serial.print(F("ERR "));
  Serial.println(msg);
}

// -----------------------------------------------------------------------------
// Status dump (same layout as v1.7 'D')
// -----------------------------------------------------------------------------
static void printMotorRow(uint8_t idx1);

// Lightweight implementation used by the API status dump. Prints one motor
// line in the same simple layout as the original HUMAN mode (M Conn Cal Max Margin).
static void printMotorRow(uint8_t idx1) {
  if (idx1 < 1 || idx1 > MAX_ACTUATORS) return;
  uint8_t i = idx1 - 1;
  char bufMax[6]; char bufMar[6];
  u16_to_cstr(max[i], bufMax);
  u16_to_cstr(margin[i], bufMar);
  char line[64];
  // Format: M Conn Cal   Max     Margin
  // Keep spacing simple and consistent.
  snprintf(line, sizeof(line), "%u   %c    %c   %5s   %5s",
           (unsigned)idx1,
           mConnected[i] ? 'Y' : 'N',
           calibrated[i] ? 'Y' : 'N',
           bufMax, bufMar);
  Serial.println(line);
}

static void apiPrintStatus(){
  // Single-line response — one \n, impossible to desync.
  // Format: STATUS FW=x.y.z BOX=n SERVO=0|1 HSPS=nnn MCMIN=x.y M1=conn,cal,max,margin ...
  // Python identifies this line by the "STATUS " prefix.
  Serial.print(F("STATUS FW=" FIRMWARE_VERSION " BOX="));
  Serial.print((uint8_t)BOX_VERSION);
  Serial.print(F(" SERVO="));
  Serial.print(servoEnabled ? '1' : '0');
  Serial.print(F(" HSPS="));
  Serial.print(getMinCalibStepsPerSecond());
  Serial.print(F(" MCMIN=" MOTION_CENTER_MIN_VERSION));
  for (uint8_t i = 0; i < MAX_ACTUATORS; i++) {
    Serial.print(F(" M"));
    Serial.print((uint8_t)(i + 1));
    Serial.print('=');
    Serial.print((uint8_t)mConnected[i]);
    Serial.print(',');
    Serial.print((uint8_t)calibrated[i]);
    Serial.print(',');
    Serial.print((unsigned)max[i]);
    Serial.print(',');
    Serial.print((unsigned)margin[i]);
    Serial.print(',');
    Serial.print((uint8_t)(axCfg[i].hometoMax ? 1 : 0));   // hdir (0=MIN,1=MAX)
    Serial.print(',');
    Serial.print((uint8_t)axCfg[i].endParkPct);            // endpark % (255=off)
  }
  Serial.println();  // unique \n that terminates the line
}

static void autoConnectIfNeeded(uint8_t motor){
  if(motor<1 || motor>MAX_ACTUATORS) return;
  uint8_t idx = motor - 1;
  if(!mConnected[idx]){
    mConnected[idx] = true;
    hwEepromWriteU16(eeAddr_P(connectedAddr, idx), 1);
    recomputeExpectedBytesP();
    updateActiveActuatorCount();
    hwLog(F("Motor was disconnected; set to CONNECTED for calibration."));
  }
}

// -----------------------------------------------------------------------------
// ENABLE / DISABLE — public handlers (also called from SerialReaderP fast-path)
// -----------------------------------------------------------------------------
static void cmdEnableCore(bool homeOptionalEvenIfUncalibrated, bool waitHomingDone, const __FlashStringHelper* doneReply) {
  syncHostConnectionState();
#ifdef DEBUG
Serial1.println("entering syncHostConnectionState");
#endif
  for (uint8_t i = 0; i < MAX_ACTUATORS; i++) {
    atomicHoldAxis(i);
  }
  enableServo();

  bool need5 = false, need6 = false;
#if MAX_ACTUATORS > 4
  need5 = mConnected[4] && !homed[4];
  if (!homeOptionalEvenIfUncalibrated) need5 = need5 && calibrated[4];
#endif
#if MAX_ACTUATORS > 5
  need6 = mConnected[5] && !homed[5];
  if (!homeOptionalEvenIfUncalibrated) need6 = need6 && calibrated[5];
#endif

  // SimHub startup diagnostics: expose optional-axis decision path before
  // homing starts so hosts can understand why intermediate states may be absent.
  if (waitHomingDone) {
#if MAX_ACTUATORS > 4
    if (!mConnected[4]) {
      Serial.println(F("M5 homing skipped: not connected"));
    } else if (!homeOptionalEvenIfUncalibrated && !calibrated[4]) {
      Serial.println(F("M5 homing skipped: not calibrated"));
    } else if (homed[4]) {
      Serial.println(F("M5 homing skipped: already homed"));
      Serial.print(F("M5 margin applied: "));
      Serial.println((unsigned)margin[4]);
    } else {
      Serial.println(F("M5 homing start"));
    }
#endif
#if MAX_ACTUATORS > 5
    if (!mConnected[5]) {
      Serial.println(F("M6 homing skipped: not connected"));
    } else if (!homeOptionalEvenIfUncalibrated && !calibrated[5]) {
      Serial.println(F("M6 homing skipped: not calibrated"));
    } else if (homed[5]) {
      Serial.println(F("M6 homing skipped: already homed"));
      Serial.print(F("M6 margin applied: "));
      Serial.println((unsigned)margin[5]);
    } else {
      Serial.println(F("M6 homing start"));
    }
#endif
  }

  if (need5 || need6) startMinCalibration(need5, need6);

  if (waitHomingDone && (need5 || need6)) {
    while (isHomingIdx(4)
#if MAX_ACTUATORS > 5
           || isHomingIdx(5)
#endif
    ) {
      tickMinCalibration();
      moveMotor();
      hwLedTick1ms();
    }

    // Explicit SH_START traceability: report each optional axis state before
    // returning CALIBRATED, so host logs show the full startup sequence.
#if MAX_ACTUATORS > 4
    if (need5) {
      if (homed[4]) {
        Serial.println(F("M5 homed"));
        Serial.print(F("M5 margin applied: "));
        Serial.println((unsigned)margin[4]);
      } else {
        Serial.println(F("M5 homing failed: MIN endstop not reached (check wiring/endstop/driver settings)"));
      }
    }
#endif
#if MAX_ACTUATORS > 5
    if (need6) {
      if (homed[5]) {
        Serial.println(F("M6 homed"));
        Serial.print(F("M6 margin applied: "));
        Serial.println((unsigned)margin[5]);
      } else {
        Serial.println(F("M6 homing failed: MIN endstop not reached (check wiring/endstop/driver settings)"));
      }
    }
#endif
  }

  Serial.println(doneReply);
}

void cmdEnable() {
  // Motion Center/API behavior: only home optional axes if fully calibrated.
  cmdEnableCore(false, false, F("OK_ENABLED"));
}

static void cmdEnableSimHub() {
  // SimHub start command: allow optional axes homing when present and wait
  // for completion before telling SimHub the platform is ready.
  cmdEnableCore(true, true, F("CALIBRATED"));
}

static void cmdDisableCore(const __FlashStringHelper* doneReply) {
  cancelMinCalibration();
  calibAbortClear();
  disableServo();
  syncHostConnectionState();
  refreshConnectionLed();
  Serial.println(doneReply);
}

void cmdDisable() {
  cmdDisableCore(F("OK_DISABLED"));
}

void shDisableFinalize() {
  // Fin differee du SH_DISABLE : procedure d'arret existante + reponse.
  cmdDisableCore(F("SH_DISABLED"));
}

static void cmdDisableSimHub() {
  cmdDisableCore(F("BOX_STOP"));
}

// Parser core
// -----------------------------------------------------------------------------
void parserInit(){ cmdLen = 0; cmd_start = 0; }

bool parserLineInProgress(){ return cmd_len > 0; }

void parserService(){
  if(cmd_len>0 && (millis() - cmd_start) > CMD_TIMEOUT){
    cmd_buf[cmd_len]=0;
    handleCommand_API(cmd_buf);
    cmd_len = 0;
  }
}

void processIncomingByte(uint8_t b){
  if(b=='\n' || b=='\r'){
    if(cmd_len>0){
      // Safety: only execute if all bytes are printable ASCII (0x20-0x7E).
      // If desynchronised P-frame data reaches the text parser, position bytes
      // (0-255) can contain 0x0A/0x0D which would otherwise fire a garbage
      // command and send back "ERR UNKNOWN_CMD", crashing the SimHub link.
      bool printable = true;
      for(uint8_t j=0; j<cmd_len; j++){
        if(cmd_buf[j]<0x20 || cmd_buf[j]>0x7E){ printable=false; break; }
      }
      if(printable){
        cmd_buf[cmd_len]=0;
        handleCommand_API(cmd_buf);
      }
      cmd_len=0;
    }
    return;
  }
  // Discard non-printable bytes (binary garbage) and flush the partial buffer
  // so that a single stray byte never corrupts the following command.
  if(b < 0x20 || b > 0x7E){ cmd_len=0; return; }
  if(cmd_len==0) cmd_start = millis();
  if(cmd_len < sizeof(cmd_buf)-1) cmd_buf[cmd_len++] = (char)b;

  if(cmd_len >= sizeof(cmd_buf)-1){
    cmd_buf[cmd_len]=0;
    handleCommand_API(cmd_buf);
    cmd_len = 0;
  }
  parserService();
}

// -----------------------------------------------------------------------------
// Soft protocol command handler (Python UI)
// -----------------------------------------------------------------------------
static void handleCommand_API(const char* cmd){
  // Work on a small mutable copy (cmdBuf is already small)
  char line[48];
  strncpy(line, cmd, sizeof(line)-1);
  line[sizeof(line)-1] = 0;
  // Trim leading spaces
  char* p = line;
  while(*p==' '||*p=='\t') ++p;
  if(*p==0) return;

  // Uppercase for easier comparisons
  strtoupper_inplace(p);

  char* cursor = p;
  char* t0 = nextTok(&cursor);
  if(!t0) return;

  // Fast compact endstop query: E5 or E 5 -> reply '1' or '0' immediately.
  // IMPORTANT: do not capture ENABLE here.
  if (t0[0]=='E' && (t0[1]=='\0' || (t0[1]>='0' && t0[1]<='9'))) {
    uint8_t motor = 0;
    if (t0[1] == '\0') {
      char* mot = nextTok(&cursor);
      if (!mot || !parseU8Tok(mot, &motor) || motor < 1 || motor > MAX_ACTUATORS) {
        apiReplyERR(F("BAD_MOTOR"));
        return;
      }
    } else {
      if (!parseU8Tok(t0+1, &motor) || motor < 1 || motor > MAX_ACTUATORS) {
        apiReplyERR(F("BAD_MOTOR"));
        return;
      }
    }
    bool state = hwReadEndstop(motor);
    // Super-light response for minimal parsing on the Python side
    Serial.println(state ? F("1") : F("0"));
    return;
  }

  // MC_START / HELLO — Motion Center opens a session.
  if(streq(t0, "MC_START") || streq(t0, "HELLO")){
    mcConnected = true;
    syncHostConnectionState();
    refreshConnectionLed();
    Serial.println(F("OK_MC_CONNECTED"));
    return;
  }

  // MC_DISABLE — Motion Center requests servo off while keeping the session.
  if(streq(t0, "MC_DISABLE")){
    mcConnected = true;
    syncHostConnectionState();
    cmdDisableCore(F("OK_MC_DISABLED"));
    return;
  }

  // SH_CONNECTED — SimHub link established; LED stays in the normal idle policy.
  if(streq(t0, "SH_CONNECTED")){
    simhubConnected = true;
    simhubState = SH_STATE_IDLE;
    syncHostConnectionState();
    refreshConnectionLed();
    Serial.println(F("SHCONNECTED"));
    return;
  }


  // SH_START — SimHub starts motion; wait for optional homing and reply CALIBRATED.
  if(streq(t0, "SH_START")){
    #ifdef DEBUG
    Serial1.println("SH_START reçu");
        #endif
    endParkCancel();   // nouvelle session : abandonner un endpark en cours
    simhubConnected = true;
    simhubState = SH_STATE_MOTION_READY;
    syncHostConnectionState();
    cmdEnableSimHub();
    return;
  }

  // SH_DISABLE — SimHub stops motion but keeps the serial session alive.
  if(streq(t0, "SH_DISABLE") ){
    // SimHub stop should return to idle (green LED) when no MC session is active.
    simhubConnected = false;
    simhubState = SH_STATE_NONE;
    syncHostConnectionState();
    // Endpark : rejoindre doucement le % de course configure (M5/M6) AVANT
    // la procedure d'arret. NON bloquant : la fin (servo off + SH_DISABLED)
    // est differee via endParkTick() dans loop().
    if (endParkBegin()) {
      return;
    }
    cmdDisableCore(F("SH_DISABLED"));
    return;
  }

  // SH_DOWN / SH-DOWN — SimHub disconnects completely.
  if(streq(t0, "SH_DOWN")){
    simhubConnected = false;
    simhubState = SH_STATE_NONE;
    syncHostConnectionState();
    refreshConnectionLed();
    Serial.println(F("BOX_DOWN"));
    return;
  }

  // MC_STOP / DISCONNECT — Motion Center clean disconnect.
  if(streq(t0, "MC_STOP") || streq(t0, "DISCONNECT")){
    mcConnected = false;
    simhubState = SH_STATE_NONE;
    syncHostConnectionState();
    refreshConnectionLed();
    Serial.println(F("OK_MC_DISCONNECTED"));
    return;
  }

  if(streq(t0, "ENABLE"))  { cmdEnable();  return; }
  if(streq(t0, "DISABLE")) { cmdDisable(); return; }

  // GET
  if(streq(t0, "GET")){
    apiPrintStatus();
    return;
  }

  // SET <KEY> <VALUE> [MOTOR]
  if(streq(t0, "SET")){
    char* key = nextTok(&cursor);
    char* val = nextTok(&cursor);
    char* mot = nextTok(&cursor);
    if(!key || !val){ apiReplyERR(F("BAD_ARGS")); return; }

    uint32_t v32 = 0;
    if(!parseU32(val, &v32)) { apiReplyERR(F("BAD_VALUE")); return; }
    if(v32 > 65535UL) v32 = 65535UL;

    uint8_t motor = 0;
    if(mot){
      if(!parseU8Tok(mot, &motor) || motor<1 || motor>MAX_ACTUATORS){ apiReplyERR(F("BAD_MOTOR")); return; }
    }

    if(streq(key, "HOMING_S")){
      // Reply OK first: EEPROM writes can briefly stall USB CDC on some builds.
      apiReplyOK();
      Serial.flush();
      delay(5);

      uint32_t sps = v32;
      if(sps < HOMING_SPS_MIN) sps = HOMING_SPS_MIN;
      if(sps > HOMING_SPS_MAX) sps = HOMING_SPS_MAX;
      uint32_t us=(1000000UL+(sps/2))/sps;
      setMinCalibSpeed(us, homingStepSizeFromSps(sps));
      hwEepromWriteU16(EEPROM_HOMINGSPS_ADDR,(uint16_t)sps);
      return;
    }

    if(motor==0){ apiReplyERR(F("MOTOR_REQUIRED")); return; }
    uint8_t idx = motor-1;

    if(streq(key, "CONNECTED")){
      uint16_t b = (v32!=0)?1:0;
      mConnected[idx] = (b!=0);
      hwEepromWriteU16(eeAddr_P(connectedAddr, idx), b);
      recomputeExpectedBytesP();
      updateActiveActuatorCount();
      apiReplyOK();
      return;
    }

    if(streq(key, "MAX")){
      uint16_t v = (uint16_t)v32;
      if(motor <= 4 && v > POSMAP_INPUT_MAX) v = POSMAP_INPUT_MAX;   // M1..M4 : unites 15 bits (profil SimHub)
      max[idx] = v;
      hwEepromWriteU16(eeAddr_P(maxAddr, idx), v);
      // Keep existing helper behavior: persist calib for optional motors
      saveCalibration(motor);
      calibrated[idx] = (max[idx] != 0);
      apiReplyOK();
      return;
    }

    if(streq(key, "MARGIN")){
      // Product rule: margins are managed only for optional motors M5/M6.
      // Motors 1..4 keep their existing configured behavior and cannot be edited via API.
      if(motor <= 4){
        apiReplyERR(F("MARGIN_ONLY_M5_M6"));
        return;
      }
      margin[idx] = (uint16_t)v32;
      hwEepromWriteU16(eeAddr_P(marginAddr, idx), (uint16_t)v32);
      saveCalibration(motor);
      apiReplyOK();
      return;
    }

    if(streq(key, "HOMING_DIR")){
      // Direction de homing (0=MIN, 1=MAX — shared/HomingDir.h).
      // Sur cette box, seuls M5/M6 ont une entree endstop lisible.
      if(motor <= 4){
        apiReplyERR(F("ONLY_M5_M6"));
        return;
      }
      bool toMax = (v32 != 0);
      if(toMax && !HomingDir_MaxAllowed(max[idx])){
        apiReplyERR(F("MAX_REQUIRED"));   // homing MAX exige un max calibre
        return;
      }
      axCfg[idx].hometoMax = toMax;
      homed[idx] = false;                 // re-homing requis avec la nouvelle direction
      hwEepromWriteU16((idx == 4) ? EEPROM_M5HOMINGDIR_ADDR
                                  : EEPROM_M6HOMINGDIR_ADDR,
                       toMax ? 1 : 0);
      apiReplyOK();
      return;
    }

    if(streq(key, "ENDPARK")){
      // Endpark % (0..100, 255=off) : position rejointe doucement au
      // SH_DISABLE avant la coupure servo. M5/M6 uniquement sur cette box.
      if(motor <= 4){
        apiReplyERR(F("ONLY_M5_M6"));
        return;
      }
      uint8_t pct = (v32 <= 100UL) ? (uint8_t)v32 : (uint8_t)ENDPARK_OFF;
      axCfg[idx].endParkPct = pct;
      hwEepromWriteU16((idx == 4) ? EEPROM_M5ENDPARK_ADDR
                                  : EEPROM_M6ENDPARK_ADDR, pct);
      apiReplyOK();
      return;
    }

    apiReplyERR(F("UNKNOWN_KEY"));
    return;
  }

  // DO <ACTION> [MOTOR]
  if(streq(t0, "DO")){
    char* act = nextTok(&cursor);
    char* mot = nextTok(&cursor);
    if(!act){ apiReplyERR(F("BAD_ARGS")); return; }
    uint8_t motor = 0;
    if(mot){
      if(!parseU8Tok(mot, &motor) || motor>MAX_ACTUATORS){ apiReplyERR(F("BAD_MOTOR")); return; }
    }

    if(streq(act, "CANCEL")){
      cancelMinCalibration();
      calibAbortRequest();
      apiReplyOK();
      return;
    }

    if(streq(act, "DETECT_MIN")){
      // v1.7-like: blocking detectMin with detailed log messages
      bool ok = true;
      if(motor==5 || motor==0){
        autoConnectIfNeeded(5);
        hwLog(F("M5"));
        ok = detectMin(5) && ok;
      }
      if(motor==6 || motor==0){
        autoConnectIfNeeded(6);
        hwLog(F("M6"));
        ok = detectMin(6) && ok;
      }
      if(motor!=0 && motor!=5 && motor!=6){ apiReplyERR(F("ONLY_M5_M6")); return; }
      if(ok) apiReplyOK(); else apiReplyERR(F("CALIB_FAILED"));
      return;
    }

    // Python-friendly non-blocking queries: simply return endstop state so
    // the Python UI can decide to keep moving or cancel from its side.
    if(streq(act, "DETECT_MIN_PY")){
      if(motor!=5 && motor!=6){ apiReplyERR(F("ONLY_M5_M6")); return; }
      bool reached = detectMinPy(motor);
      if(reached) apiReplyOK(); else apiReplyERR(F("NOT_REACHED"));
      return;
    }

    if(streq(act, "DETECT_MAX_PY")){
      if(motor!=5 && motor!=6){ apiReplyERR(F("ONLY_M5_M6")); return; }
      bool reached = detectMaxPy(motor);
      if(reached) apiReplyOK(); else apiReplyERR(F("NOT_REACHED"));
      return;
    }

    if(streq(act, "SERVO_ON"))  {
      enableServo();
      // Same rule as cmdEnable: only home if calibrated.
      bool do5 = mConnected[4] && !homed[4] && calibrated[4];
      bool do6 = false;
#if MAX_ACTUATORS > 5
      do6 = mConnected[5] && !homed[5] && calibrated[5];
#endif
      if (do5 || do6) startMinCalibration(do5, do6);
      apiReplyOK();
      return;
    }
    if(streq(act, "SERVO_OFF")) {
      cancelMinCalibration();
      calibAbortClear();
      disableServo();
      apiReplyOK();
      return;
    }

    // STEP <MOTOR> <DELTA> : increment mTarget for a single motor by signed delta
    if(streq(act, "STEP")){
      char* mot = nextTok(&cursor);
      char* dlt = nextTok(&cursor);
      if(!mot || !dlt){ apiReplyERR(F("BAD_ARGS")); return; }
      uint8_t motor = 0;
      if(!parseU8Tok(mot, &motor) || motor<1 || motor>MAX_ACTUATORS){ apiReplyERR(F("BAD_MOTOR")); return; }
      int32_t delta = 0; if(!parseS32(dlt, &delta)){ apiReplyERR(F("BAD_VALUE")); return; }
      uint8_t idx = motor - 1;
      // Adjust target safely relative to the CURRENT POSITION so a step is
      // actually requested even if mTarget was already equal to mPosition.
      int32_t cur = (int32_t)atomicGetPosition(idx);
      int32_t nxt = cur + delta;
      if(nxt < 0) nxt = 0;
      if(nxt > 65535) nxt = 65535;
      atomicSetTarget(idx, (uint16_t)nxt);
      apiReplyOK();
      return;
    }

    // DO TARGET <motor> <value> — set absolute mTarget for manual test mode.
    // Python pre-computes the mapped position; firmware just stores it.
    // No OK reply: this is a high-frequency command (similar role to P-frames).
    if(streq(act, "TARGET")){
      if(motor < 1 || motor > MAX_ACTUATORS){ return; }
      char* rawval = nextTok(&cursor);
      uint32_t v32 = 0;
      if(!rawval || !parseU32(rawval, &v32)){ return; }
      if(v32 > 65535UL) v32 = 65535UL;
      uint8_t idx = motor - 1;
      if(mConnected[idx]){
        atomicSetTarget(idx, (uint16_t)v32);
      }
      return;
    }

    if(streq(act, "FACTORY_RESET")){
      // Keep current connected flags for M5/M6
      FactoryReset(mConnected[4], mConnected[5]);
      apiReplyOK();
      return;
    }

    // Full calibration (non-interactive) for M5/M6 only
    if(streq(act, "FULL_CALIB") || streq(act, "COMPLETE_CALIB")){
      if(motor!=5 && motor!=6){ apiReplyERR(F("ONLY_M5_M6")); return; }
      autoConnectIfNeeded(motor);
      hwLog(F("M")); logU16_inline(F(""), motor);
      bool ok = fullCalibrationSoft(motor);
      if(ok) apiReplyOK(); else apiReplyERR(F("CALIB_FAILED"));
      return;
    }

    // Move to MAX (v1.7 menu item). Soft-only: uses detectMax(), then stores max/margin.
    if(streq(act, "MOVE_TO_MAX")){
      if(motor!=5 && motor!=6){ apiReplyERR(F("ONLY_M5_M6")); return; }
      autoConnectIfNeeded(motor);
      hwLog(F("M")); logU16_inline(F(""), motor);
      bool ok = detectMax(motor);
      if(!ok){ apiReplyERR(F("CALIB_FAILED")); return; }
      // Record and persist
      uint8_t idx = motor - 1;
      uint16_t pos = mPosition[idx];
      max[idx] = pos;
      calibrated[idx] = (pos != 0);
      homed[idx] = true;
      saveCalibration(motor);
      apiReplyOK();
      return;
    }

    if(streq(act, "GO_TO_CENTER") || streq(act, "MOVE_TO_CENTER") || streq(act, "CENTER")){
      if(motor!=5 && motor!=6){ apiReplyERR(F("ONLY_M5_M6")); return; }
      autoConnectIfNeeded(motor);
      hwLog(F("M")); logU16_inline(F(""), motor);
      bool ok = goToCenterSmart(motor);
      if(ok) apiReplyOK(); else apiReplyERR(F("CENTER_FAILED"));
      return;
    }

    if(streq(act, "TEST_STROKE")){
      if(motor!=5 && motor!=6){ apiReplyERR(F("ONLY_M5_M6")); return; }
      autoConnectIfNeeded(motor);
      testFullStroke(motor);
      apiReplyOK();
      return;
    }

    apiReplyERR(F("UNKNOWN_ACTION"));
    return;
  }

  apiReplyERR(F("UNKNOWN_CMD"));
}

// Public entry (Soft protocol)
void handleCommand(const char* cmd){
  handleCommand_API(cmd);
}

// Disable all HUMAN-only code: API-only/embedded build
#define API_ONLY 1
