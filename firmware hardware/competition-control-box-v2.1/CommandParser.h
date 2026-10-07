/*
 * CommandParser.h
 *
 * Soft-only protocol (v1.8): line-oriented, machine-readable.
 *
 * The Python UI is the only expected client.
 *
 * Text commands (\n-terminated):
 *   MC_START (alias HELLO)      -> OK_MC_CONNECTED
 *   MC_DISABLE                   -> OK_MC_DISABLED
 *   MC_STOP  (alias DISCONNECT) -> OK_MC_DISCONNECTED
 *   SH_CONNECTED (SHCONNECTED / SH-CONNECTED) -> SHCONNECTED
 *   SH_IDLE (SHIDLE / SH-IDLE)               -> BOX_IDLE
 *   SH_START (SHSTART / SH-START)            -> CALIBRATED
 *   SH_DISABLE (SHDISABLE / SH-DISABLE,
 *               SH_STOP / SHSTOP / SH-STOP) -> SH_DISABLED
 *   SH_DOWN (SHDOWN / SH-DOWN)               -> BOX_DOWN
 *   ENABLE                      -> OK_ENABLED
 *   DISABLE (legacy alias)      -> OK_DISABLED
 *   GET                         -> KEY=VAL lines ... END
 *   SET <KEY> <VALUE> [MOTOR]   -> OK / ERR
 *   DO  <ACTION> [MOTOR]        -> OK / ERR
 *
 * Binary targets are handled outside of this parser for real-time performance.
 */

#pragma once

#include <stdint.h>

// Initialise parser state.  Resets any internal buffers.
void parserInit();
void printMotorSummary(uint8_t idx);
// Process one incoming byte from the serial stream.  The parser supports
// line‑oriented commands terminated by newline or carriage return.  It
// accumulates characters until a line terminator or a timeout occurs.  The
// recognised command formats include:
//   D      — display global status
//   D5     — display status for motor 5
//   F:CAL5 — perform full calibration on motor 5
//   F:CAL6 — perform full calibration on motor 6
//   P:M5MAX?      — print current maximum travel for motor 5
//   P:M5MAX=12345 — update maximum travel value for motor 5 and save
// Additional commands can be added in CommandParser.cpp.  Binary 'T'
// commands should be handled outside of this parser to avoid delay.
void processIncomingByte(uint8_t b);
// Flush a partial text command when no newline was sent and the
// inter-byte timeout has elapsed.
void parserService();
// True while a text line is being accumulated (bytes received, no EOL yet).
// Used by SerialReaderP() : un octet 'P' au MILIEU d'une ligne texte (ex.
// "SET ENDPARK 75 5", "DO DETECT_MIN_PY 5") ne doit PAS etre interprete
// comme un tag de trame binaire, sinon il avale 15 octets de texte.
bool parserLineInProgress();
void handleCommand(const char* cmd);
bool detectMin(uint8_t motor);

// Called directly from SerialReaderP() for fast-path processing at the
// same level as the 'P' binary frame, and also from handleCommand_API.
void cmdEnable();
void cmdDisable();

// Deferred SH_DISABLE completion : runs the normal stop procedure and
// replies SH_DISABLED.  Called by endParkTick() when the endpark move
// finishes (or aborts/times out).
void shDisableFinalize();

