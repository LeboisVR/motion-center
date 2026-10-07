/*
 * CalibrationUtils.h
 *
 * Code version : 1.0
 *
 * This header defines a set of helper functions for calibrating the limit
 * switches and travel range of actuators on the competition control box.  It
 * provides a high level API for performing full calibration on a given motor
 * (finding min, max and applying a margin), loading and saving calibration
 * values to EEPROM, and testing the full stroke.  These functions build on
 * top of the MotorControl API and use the HardwareAbstraction layer for
 * interacting with the EEPROM and LEDs.
 */

#pragma once

#include <stdint.h>
#include "Globals.h"

// EEPROM offsets for storing calibration data.  Each motor stores its max
// travel (including margins) and the chosen margin.  Slots for motor 5 and 6
// are provided.  Additional motors can be added by allocating more slots.


// Calibration values are stored in global arrays declared in Globals.h:
// `max[]` holds the maximum travel (stroke) for each actuator and
// `margin[]` holds the safety margin for each actuator.  Motors 5 and 6
// correspond to indices 4 and 5 in these arrays.  The calibration
// functions update these entries directly when a new calibration is
// performed.

// Initialise calibration subsystem.  This should configure the endstop pins
// through the HardwareAbstraction layer and load any previously saved
// calibration data from EEPROM.


// Load calibration values for the given motor from EEPROM.  If the values
// stored in EEPROM are invalid (e.g. zero or out of range) they are left
// unchanged.  Motor must be 5 or 6.
void loadCalibration(uint8_t motor);

// Save calibration values for the given motor to EEPROM.  This writes both
// the maximum travel and the margin.  Motor must be 5 or 6.
void saveCalibration(uint8_t motor);

// Prepare to calibrate a given motor.  Displays a prompt and waits for the
// user to send 's' to start.  Activates the servo and sets the LED to red.
void prepareCalibration(uint8_t motor);

// Move the given motor until the minimum position endstop is detected.  The
// motor is moved in the compress direction until the endstop becomes active
// (and then released slightly).  The position is reset to zero.
void moveToMin(uint8_t motor);

// Move the given motor until the maximum position endstop is detected.  The
// motor is moved in the decompress direction until the endstop becomes
// active, then reversed slightly.  Updates the corresponding entry in the
// `max[]` array (index 4 for motor 5 or 5 for motor 6).
void moveToMax(uint8_t motor);

// Endpark (SH_DISABLE) : moves M5/M6 slowly (homing speed) to
// axCfg[].endParkPct % of the usable stroke (max - 2*margin) before the
// servo power is cut.  NON-BLOCKING state machine:
//  - endParkBegin() computes the targets and arms the FSM (returns false
//    when nothing to do : servo off / endpark off / not homed).
//  - endParkTick() must be called from loop(); when the move completes
//    (or aborts/times out) it calls shDisableFinalize() which runs the
//    normal stop procedure and replies SH_DISABLED.
bool endParkBegin();
void endParkTick();
bool endParkActive();
void endParkCancel();

bool detectMin(uint8_t motor); 

// Soft endstop-based MAX detection (used by the Python UI).
// Supported on M5/M6 only. Returns true on success.
bool detectMax(uint8_t motor);

// Python-friendly non-blocking endstop queries.
// These return the instantaneous endstop state for motor 5 or 6.
// Used by the Python UI to decide whether to continue moving the actuator.
bool detectMinPy(uint8_t motor);
bool detectMaxPy(uint8_t motor);

// Ask the user to enter a margin value for the given motor.  Applies the
// margin temporarily and prompts for confirmation.  If the user confirms
// (replying 'y') the margin is stored in the corresponding `margin[]`
// entry and the maximum travel is reduced accordingly in `max[]`.  If not
// confirmed the values are restored and the user is prompted again.  This
// function uses the serial monitor for interactive input and output.
void askAndConfirmMargin(uint8_t motor);

// Perform a full stroke test on the given motor.  Moves the motor to zero,
// then to max, then back to mid‑stroke.  This is useful to ensure that the
// margin and max values are sensible after calibration.
void testFullStroke(uint8_t motor);

// Perform a full calibration on the given motor: prepare, find min, find max,
// prompt for margin, save the calibration and test the stroke.  At the end
// the servo is disabled and the LED set to green.  Suitable for motors with
// mechanical endstops.  This function may block while waiting for user
// input.
void fullCalibration(uint8_t motor);
// Soft-only variant: no interactive prompts / no "send s".
// Returns true on success.
bool fullCalibrationSoft(uint8_t motor);

// Move to effective center. If position is unknown, detectMin is run first.
// Supported on M5/M6 only.
bool goToCenterSmart(uint8_t motor);
