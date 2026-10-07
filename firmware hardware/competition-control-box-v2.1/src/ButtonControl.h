/*
 * ButtonControl.h
 *
 * Declares a small interface for managing physical buttons on the
 * competition control box.  When the firmware is compiled with
 * BUTTONMOD defined three buttons are available to adjust traction loss
 * gain or toggle the SimHub connection.  This module encapsulates the
 * button state machine and debouncing logic so that the main sketch
 * remains uncluttered.
 */

#pragma once

#ifdef BUTTONMOD

#include <Arduino.h>
#include "src/Joystick.h"
Joystick_ Joystick(0x06, JOYSTICK_TYPE_JOYSTICK, 3);
// ---------------------------------------------------------------------------
// Button pin definitions
//
// The IncreaseGain, DecreaseGain and ConnectPin signals are wired to
// specific analogue input pins on the Arduino Leonardo.  Historically these
// pins were defined in the main sketch via preprocessor defines.  Moving
// them into this header centralises the configuration and avoids leaking
// hardware details into unrelated files.  You can override these defaults
// at compile time by defining IncreaseGain, DecreaseGain and/or ConnectPin
// before including this header.  The default assignments correspond to
// A1, A2 and A3 respectively.
#ifndef IncreaseGain
#define IncreaseGain A1
#endif
#ifndef DecreaseGain
#define DecreaseGain A2
#endif
#ifndef ConnectPin
#define ConnectPin   A3
#endif

// Initialise the button handler.  Pass in the pin numbers for the
// increase, decrease and connect buttons.  The Joystick reference is
// used to send button states back to the host.
void buttonInit(uint8_t increasePin, uint8_t decreasePin, uint8_t connectPin);

// Poll the buttons and send any state changes via the provided joystick.
// Call this from the main loop when BUTTONMOD is enabled.
void buttonUpdate(Joystick_ &joystick);

#endif // BUTTONMOD