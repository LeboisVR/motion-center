/*
// www.lebois-racing.com
// 4 actuators SRT80
// competition control box (3 buttons)
// Leonardo only
// v1.0  - 23/08/03
// v1.7  - calibration code integrated in the main code and m6 added
*/

// ---------- Includes ----------
#include <Arduino.h>
#include "CommandParser.h"
#include "CalibrationUtils.h"
#include "MotorControl.h"
#include "HardwareAbstraction.h"
#include "Globals.h"
#include "MiscUtils.h"
#include "MinCalibration.h"
#include "Debug.h"   // DBG / DBG_VAL / DBG_CHANGE — no-op en release
#include "PosMap.h"  // librairie commune : mapping consigne 15 bits -> cible
#include "PFrame.h"  // librairie commune : format trame 'P' (7 axes, 15 octets)

// Pas physiques par unite 15 bits sur les axes de base M1..M4 (AVR :
// course lineaire sans facteur supplementaire sur la consigne 15 bits).
#define BASE_AXIS_STEP_SCALE 1U

// ---------- States & shared data ----------
// True when any host (Python UI via HELLO, or SimHub via ENABLE) has an active session.
bool hostConnected = false;
// True when Motion Center session is active (MC_CONNECTED).
bool mcConnected = false;
// True when the host session is established by SimHub via SH_START.
bool simhubConnected = false;
SimHubState simhubState = SH_STATE_NONE;
// Timestamp of the last API command received from the host
unsigned long lastApiTime = 0;
// Timeout (ms) without any host command before considering disconnected
static const unsigned long HOST_TIMEOUT_MS = 30000UL;
// If P-frames pause for a while (typical when switching games), ignore the
// first resumed frame to prevent a startup "kick" in the wrong direction.
static const unsigned long PFRAME_RESUME_GUARD_MS = 250UL;
// Fail-safe: if SimHub was actively driving motion and the P stream stalls,
// start a short park-to-zero, then force servo off instead of waiting for the
// generic host timeout.
static const unsigned long SIMHUB_STREAM_TIMEOUT_MS = 1200UL;
static const unsigned long SIMHUB_FAILSAFE_PARK_MAX_MS = 3000UL;
static unsigned long lastPFrameMs = 0;
static bool simhubFailsafeParking = false;
static unsigned long simhubFailsafeStartMs = 0;
// Hold base axes (M1-M4) during optional-axis auto-homing (M5/M6), and keep
// them held until at least one valid P-frame is received after homing ends.
static bool holdBaseAxesAfterHoming = false;

// Les buffers/états globaux sont définis dans Globals.cpp
// Assure-toi que Globals.h les déclare en 'extern'.
extern uint16_t          margin   [MAX_ACTUATORS];
extern uint16_t          max      [MAX_ACTUATORS];

extern bool mConnected [MAX_ACTUATORS];   // nouveau : moteurs présents
extern bool calibrated [MAX_ACTUATORS];   // nouveau : min/max connus
extern bool homed      [MAX_ACTUATORS];   // nouveau : zéro posé
extern bool homing     [MAX_ACTUATORS];   // nouveau : homing en cours
extern bool servoEnabled;                 // défini dans MotorControl.cpp

// ---------- Protos ----------
static void startSimHubFailsafeParkToZero();
static bool simhubFailsafeParkReachedTargets();

void syncHostConnectionState() {
  hostConnected = mcConnected || simhubConnected;
}

void refreshConnectionLed() {
  if (servoEnabled) return;
  if (hostConnected) {
    hwLedSet("blue");
  } else {
    hwLedSet("green");
  }
}

void serviceHostLink() {
  static bool wasConnected = false;
  // ATTENTION : ne JAMAIS utiliser `(bool)Serial` ici. Sur ATmega32u4,
  // Serial_::operator bool() du core Arduino contient un delay(10)
  // INCONDITIONNEL -> loop() plafonne a ~100 Hz -> ~100 pas/s en bit-bang
  // (mouvement "hyper lent"). Serial.dtr() lit le meme etat DTR sans delai.
  bool nowConnected = Serial.dtr();   // état DTR du CDC (sans delay(10))

  if (wasConnected && !nowConnected) {
    DBG("host: disconnected");
    // L'hôte a disparu (reboot/fermeture du port) -> on nettoie tout
    parserInit();                              // vide la commande texte partielle
    while (Serial.available()) Serial.read();  // jette le 'P' orphelin
    simhubConnected = false;
    mcConnected     = false;
    simhubState     = SH_STATE_NONE;
    lastPFrameMs    = 0;
    syncHostConnectionState();
    disableServo();
    refreshConnectionLed();
  }
  if (!wasConnected && nowConnected) {
    DBG("host: connected");
    // Reconnexion fraîche -> on s'assure qu'aucun octet d'avant ne traîne
    parserInit();
    while (Serial.available()) Serial.read();
  }
  wasConnected = nowConnected;
}

// ================================================================
// Setup
// ================================================================
void setup() {
    motorInit();
    motorTimerInit();
    disableServo();       // au repos au boot
  delay(100);
  hwLedInit();

uint16_t mark = hwEepromReadU16(EEPROM_HAS_BEEN_FACTORYRESET);
if (mark != EEPROM_SCHEMA_VERSION) {  // mismatch = first boot after update or blank chip
  FactoryReset(false, false);          // M5/M6 déconnectés par défaut — à activer via Motion Center
}

  Serial.begin(115200);
  hwDebugInit();
  DBG("boot");
  loadConfig();

  updateActiveActuatorCount();
  motorTimerInit();  // bit-bang loop : s'assure que l'ISR Timer3 est désarmée

  // Soft-only: keep startup quiet. The Python UI will query with HELLO/GET.
  syncHostConnectionState();
  refreshConnectionLed();

}

// ================================================================
// Loop
// ================================================================
void loop() {
  serviceHostLink();

  if (Serial.available()) lastApiTime = millis();  // any serial activity = host alive
  const bool homing56_before = isHomingIdx(4) || isHomingIdx(5);
  if (homing56_before) {
    holdBaseAxesAfterHoming = true;
  }

  const bool steppedFromTargetFrame = SerialReaderP();
  parserService();

  const bool homing56_after = isHomingIdx(4) || isHomingIdx(5);
  if (holdBaseAxesAfterHoming && !homing56_after && steppedFromTargetFrame) {
    holdBaseAxesAfterHoming = false;
  }

  tickMinCalibration();

  // Endpark FSM (SH_DISABLE) : pilotage doux vers le % configure, fin
  // differee (servo off + SH_DISABLED) sans bloquer le protocole.
  endParkTick();

  if (homing56_after || holdBaseAxesAfterHoming) {
    for (uint8_t i = 0; i < 4 && i < MAX_ACTUATORS; i++) {
      if (!mConnected[i]) continue;
      atomicHoldAxis(i);
    }
  }

  // Bit-bang v1.5 : un pas par passe de loop, puis retour a SerialReaderP
  // pour rafraichir les cibles. Vitesse max = frequence de la boucle.
    // Stepping is handled by Timer3 ISR; loop() must not emit steps.

  // Keep non-blocking LED animations responsive.
  hwLedTick1ms();

  // SimHub fail-safe: while motion is active, loss of P frames usually means
  // the game/sender stalled. Park to zero first, then cut servo power.
  if (simhubConnected && servoEnabled && simhubState == SH_STATE_MOTION_ACTIVE) {
    if (lastPFrameMs != 0 && (millis() - lastPFrameMs > SIMHUB_STREAM_TIMEOUT_MS)) {
      startSimHubFailsafeParkToZero();
    }
  }

  if (simhubFailsafeParking && simhubConnected && servoEnabled) {
    bool reached = simhubFailsafeParkReachedTargets();
    bool expired = (millis() - simhubFailsafeStartMs) > SIMHUB_FAILSAFE_PARK_MAX_MS;
    if (reached || expired) {
      if (reached) {
        hwLog(F("[SAFE] SimHub PARK_ZERO done -> SERVO_OFF"));
      } else {
        hwLog(F("[SAFE] SimHub PARK_ZERO timeout -> SERVO_OFF"));
      }
      simhubFailsafeParking = false;
      disableServo();
      simhubConnected = false;
      simhubState = SH_STATE_NONE;
      syncHostConnectionState();
      refreshConnectionLed();
    }
  }

  // Detect host disconnection by timeout
  if (hostConnected && (millis() - lastApiTime > HOST_TIMEOUT_MS)) {
    mcConnected = false;
    simhubConnected = false;
    simhubState = SH_STATE_NONE;
    syncHostConnectionState();
    if (servoEnabled) {
      disableServo();   // cut relay if host disappeared without sending DISABLE
    } else {
      refreshConnectionLed();
    }
  }
}

// ================================================================
// Lecture série + protocole
//
// Deux formats coexistent sur le même port série :
//   Binaire : 'P' + 14 octets exactement  (7 axes × 2 octets big-endian, 15 bits)
//   Texte   : commandes terminées par '\n' (HELLO, ENABLE, GET, SET…)
//
// Règle d'or : on ne lit 'P' que si 1 + 7*2 octets sont disponibles,
// donc jamais de désynchronisation possible.
// ================================================================
// SerialReaderP — version adaptée à l'architecture métronome Timer3.
//
// Changements vs version précédente :
//   1. Drainage par compte : Serial.available() est appelé UNE fois par
//      passage (chaque appel traverse la pile USB CDC du 32U4). Ce qui
//      arrive pendant le traitement sera pris au passage suivant de loop(),
//      ce qui borne aussi le temps passé ici.
//   2. Plus d'appels moveMotor() : les pas sont émis par l'ISR Timer3.
//      (Ils étaient devenus des no-ops grâce au garde-fou OCIE3A, mais les
//      garder laissait une porte ouverte pendant la calibration, ISR masquée.)
//   3. Commentaire de cadence corrigé (20 kHz sur 32U4, pas 80 kHz).
 
bool SerialReaderP() {
  static uint8_t payload[PFRAME_PAYLOAD_LEN]; // toujours 7 axes × 2 octets (profil SimHub unique)
 
  int n = Serial.available();   // un seul aller-retour CDC par passage
 
  while (n > 0) {
    int c = Serial.peek();
    if (c < 0) return false;
 
    // -------------------------------------------------------------------
    // Trame binaire : format commun shared/PFrame.h ('P' + 7 axes × 2 octets BE,
    // 15 bits). Les axes au-delà de MAX_ACTUATORS sont consommés et ignorés ;
    // les moteurs non connectés gardent leur target.
    // GARDE : un 'P' au MILIEU d'une ligne texte en cours (ex. "SET ENDPARK
    // 75 5") n'est PAS un tag de trame — sans cette garde, le parseur avalait
    // 15 octets de texte (commande tronquée -> ERR BAD_ARGS, reglage perdu).
    // -------------------------------------------------------------------
    if (c == PFRAME_TAG && !parserLineInProgress()) {
      if (n < (int)PFRAME_LEN) {
        // Trame incomplète dans notre décompte : re-vérifier une fois si la
        // suite est arrivée pendant le drainage, sinon attendre le prochain
        // passage de loop().
        n = Serial.available();
        if (n < (int)PFRAME_LEN) return false;
      }
      Serial.read();  // consomme le tag
      for (uint8_t i = 0; i < PFRAME_PAYLOAD_LEN; i++) payload[i] = (uint8_t)Serial.read();
      n -= (int)PFRAME_LEN;
 
      unsigned long nowMs = millis();
      bool resumedAfterGap = (lastPFrameMs != 0) && ((nowMs - lastPFrameMs) > PFRAME_RESUME_GUARD_MS);
      lastPFrameMs = nowMs;
      if (simhubFailsafeParking) {
        simhubFailsafeParking = false;
        hwLog(F("[SAFE] SimHub stream resumed during PARK_ZERO"));
      }
      if (simhubConnected) {
        simhubState = SH_STATE_MOTION_ACTIVE;
      }
 
      if (resumedAfterGap) {
        // Hold base actuators for one frame after stream resume. This avoids
        // occasional wrong-direction forcing when SimHub changes game/source.
        // NB : atomicHoldAxis doit faire "target = pos" dans UN SEUL bloc
        // atomique (lecture pos + écriture target sous les mêmes IRQ
        // masquées), sinon l'ISR peut glisser un pas entre les deux.
        for (uint8_t i = 0; i < 4 && i < MAX_ACTUATORS; i++) {
          if (!mConnected[i]) continue;
          atomicHoldAxis(i);
        }
        return true;   // le métronome Timer3 fait le reste
      }
 
      for (uint8_t i = 0; i < 6; i++) {
        if (i >= MAX_ACTUATORS) break;        // sécurité tableau Box V1 (max 5 slots)
        if (!mConnected[i]) continue;          // moteur absent : target inchangée
        if (i >= 4 && !homed[i]) continue;     // axes optionnels : attendre le homing
        uint16_t u = PFrame_Axis(payload, i);  // consigne 15 bits (0..32767)
        uint16_t tgt;
        if (max[i] != 0) {
          // Mapping commun (shared/PosMap.h) :
          //  - M1..M4 : max/margin en unites 15 bits -> pas physiques (x2)
          //  - M5/M6  : max/margin en pas mesures, course utile = max - 2*margin
          tgt = (i >= 4)
              ? PosMap_TargetOptional(u, (uint16_t)max[i], margin[i])
              : PosMap_TargetBase(u, (uint16_t)max[i], margin[i], BASE_AXIS_STEP_SCALE);
        } else {
          tgt = u;
        }
        atomicSetTarget(i, tgt);   // écriture 16 bits atomique (ISR 20 kHz lit mTarget)
      }

      return true;
    }
 
    // -------------------------------------------------------------------
    // Tout le reste : commandes texte (HELLO, ENABLE, DISABLE, GET, SET…)
    // Le parseur ligne par ligne gère tout, pas besoin de chemin rapide.
    // -------------------------------------------------------------------
    processIncomingByte((uint8_t)Serial.read());
    n--;
  }
 
  return false;
}
 

bool SerialReaderPBackUp() {
  static uint8_t payload[PFRAME_PAYLOAD_LEN]; // toujours 7 axes × 2 octets (profil SimHub unique)

  while (Serial.available()) {
    int c = Serial.peek();
    if (c < 0) return false;

    // -------------------------------------------------------------------
    // Trame binaire : format commun shared/PFrame.h ('P' + 7 axes × 2 octets BE,
    // 15 bits). Les axes au-delà de MAX_ACTUATORS sont consommés et ignorés ;
    // les moteurs non connectés gardent leur target.
    // -------------------------------------------------------------------
    if (c == PFRAME_TAG) {
      if (Serial.available() < (int)PFRAME_LEN) return false;  // attendre la trame complète
      Serial.read();  // consomme le tag
      for (uint8_t i = 0; i < PFRAME_PAYLOAD_LEN; i++) payload[i] = (uint8_t)Serial.read();

      unsigned long nowMs = millis();
      bool resumedAfterGap = (lastPFrameMs != 0) && ((nowMs - lastPFrameMs) > PFRAME_RESUME_GUARD_MS);
      lastPFrameMs = nowMs;
      if (simhubFailsafeParking) {
        simhubFailsafeParking = false;
        hwLog(F("[SAFE] SimHub stream resumed during PARK_ZERO"));
      }
      if (simhubConnected) {
        simhubState = SH_STATE_MOTION_ACTIVE;
      }

      if (resumedAfterGap) {
        // Hold base actuators for one frame after stream resume. This avoids
        // occasional wrong-direction forcing when SimHub changes game/source.
        for (uint8_t i = 0; i < 4 && i < MAX_ACTUATORS; i++) {
          if (!mConnected[i]) continue;
          atomicHoldAxis(i);
        }
        return true;
      }

      for (uint8_t i = 0; i < 6; i++) {
        if (i >= MAX_ACTUATORS) break;        // sécurité tableau Box V1 (max 5 slots)
        if (!mConnected[i]) continue;          // moteur absent : target inchangée
        if (i >= 4 && !homed[i]) continue;     // axes optionnels : attendre le homing
        uint16_t u = PFrame_Axis(payload, i);  // consigne 15 bits (0..32767)
        uint16_t tgt;
        if (max[i] != 0) {
          // Mapping commun (shared/PosMap.h) :
          //  - M1..M4 : max/margin en unites 15 bits -> pas physiques (x2)
          //  - M5/M6  : max/margin en pas mesures, course utile = max - 2*margin
          tgt = (i >= 4)
              ? PosMap_TargetOptional(u, (uint16_t)max[i], margin[i])
              : PosMap_TargetBase(u, (uint16_t)max[i], margin[i], BASE_AXIS_STEP_SCALE);
        } else {
          tgt = u;
        }
        atomicSetTarget(i, tgt);   // écriture 16 bits atomique (ISR 20 kHz lit mTarget)
      }
      
      return true;
    }

    // -------------------------------------------------------------------
    // Tout le reste : commandes texte (HELLO, ENABLE, DISABLE, GET, SET…)
    // Le parseur ligne par ligne gère tout, pas besoin de chemin rapide.
    // -------------------------------------------------------------------
    processIncomingByte((uint8_t)Serial.read());
  }

  return false;
}

// ================================================================
// Utils
// ================================================================

static void startSimHubFailsafeParkToZero() {
  if (simhubFailsafeParking) return;
  hwLedSet("white");

  // Equivalent of a continuous SimHub frame stream at value 0 on all axes.
  // Base axes keep margin safety (lo=margin), optional axes use their own lo.
  for (uint8_t i = 0; i < MAX_ACTUATORS; i++) {
    if (!mConnected[i]) continue;
    if (max[i] != 0) {
      // Park-to-zero direct : pour la consigne 0, on n'a pas besoin de
      // passer par le mapping de plage; la cible est déjà la limite basse.
      atomicSetTarget(i, (i >= 4)
                 ? PosMap_TargetOptional(0, (uint16_t)max[i], margin[i])
                 : 0U);
    } else {
      atomicSetTarget(i, 0);
    }
  }

  simhubFailsafeParking = true;
  simhubFailsafeStartMs = millis();
  simhubState = SH_STATE_MOTION_READY;
}

static bool simhubFailsafeParkReachedTargets() {
  for (uint8_t i = 0; i < MAX_ACTUATORS; i++) {
    if (!mConnected[i]) continue;
    if (!atomicAxisAtTarget(i)) return false;
  }
  return true;
}