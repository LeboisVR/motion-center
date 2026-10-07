/*
 * ButtonControl.cpp
 *
 * Provides a simple button management layer for the optional three
 * buttons on the competition control box.  Buttons are debounced and
 * translated into joystick button presses.  By isolating this logic
 * here we avoid polluting the main sketch with hardware specific
 * details and make it easier to port to other platforms.
 */

#include "ButtonControl.h"

#ifdef BUTTONMOD
#include <Arduino.h>

// Store the assigned pin numbers and button state.  These variables
// are static so they retain their values across calls to buttonUpdate().
static uint8_t s_btnPins[3] = {0, 0, 0};
static unsigned long s_lastPush[3] = {0, 0, 0};
static uint8_t s_lastState[3] = {0, 0, 0};

// Debounce delay in milliseconds.  Adjust as needed to suit the
// mechanical characteristics of the buttons.
static const unsigned long DEBOUNCE_MS = 100;

void buttonInit(uint8_t increasePin, uint8_t decreasePin, uint8_t connectPin, Joystick_ &joystick) {
  // Record the pins for later use and configure them as inputs with
  // pull‑ups enabled.  Buttons are considered pressed when the pin
  // reads LOW.
  s_btnPins[0] = increasePin;
  s_btnPins[1] = decreasePin;
  s_btnPins[2] = connectPin;
  for (int i = 0; i < 3; i++) {
    pinMode(s_btnPins[i], INPUT_PULLUP);
    s_lastPush[i] = 0;
    s_lastState[i] = 0;
    // Initialise joystick buttons to released state
    joystick.setButton(i, 0);
  }
  // Immediately send the initial state
  joystick.sendState();
}

void buttonUpdate(Joystick_ &joystick) {
  unsigned long now = millis();
  for (byte i = 0; i < 3; i++) {
    // Buttons are active low
    bool pressed = (digitalRead(s_btnPins[i]) == LOW);
    if (pressed && s_lastState[i] == 0) {
      // Button has just been pressed
      s_lastState[i] = 1;
      joystick.setButton(i, 1);
      joystick.sendState();
      s_lastPush[i] = now;
    }
    if (!pressed && s_lastState[i] == 1) {
      // Button was released; apply debounce before sending release
      if (now - s_lastPush[i] > DEBOUNCE_MS) {
        s_lastState[i] = 0;
        joystick.setButton(i, 0);
        joystick.sendState();
      }
    }
  }
}

#endif // BUTTONMOD