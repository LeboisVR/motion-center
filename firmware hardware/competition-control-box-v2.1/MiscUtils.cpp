/*
 * MiscUtils.cpp
 * Code version 1.0
 * Implements miscellaneous helper functions declared in MiscUtils.h.
 * These functions provide behaviours that were previously defined
 * directly in the main sketch file.  Moving them here improves
 * organisation and allows them to be reused by other modules.
 */

#include "MiscUtils.h"
#include "HardwareAbstraction.h"
#include "MotorControl.h"
#include <Arduino.h>

// Disable the servo power and flash the LED until reset.  This
// function never returns.  It is intended to be called when a
// hardware security condition is triggered.
void Security() {
  // Turn off the servo power using the HAL wrapper.  This call
  // disables the relay controlling the actuator drivers.
  disableServo();
  // Continuously flash the LED green/red.  Use delay() rather than
  // hwDelayMs() here because this is called only in an error state and
  // blocking behaviour is acceptable.  The delays can be tuned as
  // desired.
  while (true) {
    hwLedSet("green");
    delay(200);
    hwLedSet("red");
    delay(200);
  }
}

