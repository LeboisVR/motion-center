/*
 * MotorControl.h
 *
 * Code version 1.0
 *
 * This header defines a lightweight motor control abstraction for the competition
 * control box. The goal of this module is to decouple low‑level pin
 * manipulations from the higher level calibration and parser logic. All
 * functionality that interacts directly with step and direction pins lives
 * here.  It is designed to be portable across microcontrollers by relying on
 * the HardwareAbstraction layer for delays and logging.  See
 * HardwareAbstraction.h for more details.
 */

#pragma once

#include <stdint.h>
#include "Globals.h"

// Maximum number of actuators supported.  The default build uses four
// actuators plus one traction loss axis.  If you enable a sixth actuator the
// limit should be updated accordingly.  Pins for actuators beyond the fifth
// must be added to the StepPins and DirPins arrays in the .cpp file.

void updateActiveActuatorCount();
// Public state for each actuator.  These arrays are indexed by actuator
// number minus one (motor 1 is index 0, motor 6 is index 5).  They are
// declared extern here and defined in MotorControl.cpp.  Other modules (e.g.
// calibration, parser) read and write to these variables directly.

extern  int8_t   actuatorDir[MAX_ACTUATORS];

// Expose the step and direction pin assignments.  These arrays are defined
// in MotorControl.cpp and provide the mapping from motor index to Arduino
// pin number.  Declaring them here allows other modules (e.g. calibration
// helpers) to access the pin numbers without re‑defining them.  Note that
// only the first Actuators_Count entries are actually used; the remaining
// entries correspond to optional axes.
//
// N.B. The arrays are intentionally **not** declared const.  In C++ a
// namespace‑scope const variable has internal linkage unless there is an
// extern declaration and definition in the same translation unit, which can
// lead to "undefined reference" errors when linking.  By making them
// non‑const here (and in their definition) they acquire external linkage
// automatically.  The values are still treated as read‑only by convention.
extern uint8_t StepPins[MAX_ACTUATORS];
extern uint8_t DirPins [MAX_ACTUATORS];

// API functions

// Initialise all Step and Dir pins.  Must be called from setup().  This
// function sets the pinMode for each StepPin and DirPin to OUTPUT and
// configures the direction pins to a known default value (HIGH).  On
// non‑Arduino platforms this function should be implemented accordingly.
void motorInit();
void motorTimerInit();
void startMotionTimer(uint16_t interval_us);
void stopMotionTimer();
void enterCalibrationMotionMode();
void exitCalibrationMotionMode();
bool isCalibrationMotionModeActive();

void moveMotor();
static inline void singleStep(uint8_t count);
static inline void directionManager(uint8_t count);

// Move a specific motor a given number of microsteps in the specified
// direction.  `motor` is in the range [1,MAX_ACTUATORS], `steps` is the
// distance in microsteps and `direction` is "compress" or "decompress".  The
// function updates actuatorTarget and loops calling moveMotor() until the
// requested number of steps have been completed.  The calibrationSpeed
// constant from the main sketch determines the delay between steps.
void moveSteps(uint8_t motor, long steps, const char* direction);
void enableServo();
void disableServo();
void loadConfig();
