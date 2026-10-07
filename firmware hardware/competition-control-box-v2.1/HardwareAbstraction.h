/*
 * HardwareAbstraction.h
 *Code version 1.0
 *
 * This header defines a minimal hardware abstraction layer for the
 * competition control box project.  The goal of this module is to
 * encapsulate all board‑specific operations (pins, LEDs, EEPROM and
 * endstop access) behind a small API.  By centralising these calls,
 * the main firmware and supporting libraries can remain portable
 * between the Arduino Leonardo (ATmega32u4) and alternative
 * microcontrollers such as STM32.  When targeting a new platform you
 * need only update HardwareAbstraction.cpp to provide the
 * appropriate implementations for the functions declared here.
 */

#pragma once


#include <Arduino.h>
#include <stdint.h>

//-------------------------------------------------------------------------
// Target selection
//
// Detect the target MCU at compile time.  When compiling with the
// Arduino IDE for a Leonardo the ARDUINO_AVR_LEONARDO macro is
// defined.  When compiling for an STM32 the appropriate core
// definition (e.g. STM32F4xx) should be defined instead.  These
// definitions allow HardwareAbstraction.cpp to select the correct
// implementation.
//-------------------------------------------------------------------------
#if defined(ARDUINO_AVR_LEONARDO)
#define TARGET_LEONARDO
#elif defined(STM32F4xx)
#define TARGET_STM32
#endif

//-------------------------------------------------------------------------
// LED abstractions
//
// Two board types are supported: a simple single‑colour LED (BOX v1)
// and a Pololu RGB LED strip (BOX v2).  The LedType enumeration
// distinguishes the two so that the driver can select between
// digitalWrite and RGB control.  LedColor provides a simple set of
// colours that can be requested.  Additional colours can be added as
// needed.  For backwards compatibility an overload accepting a
// C‑string is also provided; this converts the string to a LedColor
// internally.
//-------------------------------------------------------------------------
enum LedColor {
    LED_OFF,
    LED_GREEN,
    LED_RED,
    LED_ORANGE,
    LED_BLUE,
    LED_WHITE
};

enum LedType {
    LED_SIMPLE,    // Single on/off LED (BOX v1)
    LED_POLOLU     // Pololu RGB LED strip (BOX v2)
};

//-------------------------------------------------------------------------
// Endstop configuration
//
// Each endstop (limit switch) may require a pull‑up resistor and may
// be active‑low or active‑high.  The EndstopConfig structure
// encapsulates this information.  HwCalibrationPins holds the
// configuration for both the M5 and M6 endstops.  Pass this
// structure to hwInitCalibrationPins() at startup.
//-------------------------------------------------------------------------
struct EndstopConfig {
    uint8_t pin;
    bool usePullup;
    bool activeLow;
};

struct HwCalibrationPins {
    EndstopConfig m5;
    EndstopConfig m6;
};

//-------------------------------------------------------------------------
// API functions
//-------------------------------------------------------------------------
// Call this once at the very start of setup() to initialise the
// hardware abstraction layer.  On Arduino this will initialise the
// Serial port for logging.  On other platforms you may choose a
// different logging mechanism.
void hwInit();

// Configure calibration (endstop) pins.  Pass in a structure
// describing the M5 and M6 limit switch pins and their pull‑up /
// polarity requirements.  The pins will be configured as inputs
// accordingly.  You can call this function with a default
// constructed HwCalibrationPins to use the legacy built‑in pin
// assignments on Arduino.
void hwInitCalibrationPins();

// Initialise the status LED.  Provide the LedType (LED_SIMPLE for
// BOX v1, LED_POLOLU for BOX v2) to select between simple on/off
// control or full colour control.
void hwLedInit();

// Set the status LED to one of the predefined colours.  If the
// board type only supports a single colour the driver will choose
// between on/off based on the requested colour (e.g. RED maps to
// LED on, GREEN maps to LED off).  The overload accepting a
// C‑string allows backwards compatibility with existing calls such
// as setLed("red").
void hwLedSet(LedColor color);
void hwLedSet(const char *color);

// Flash the LED blue a number of times.  Useful for debugging or
// indicating progress during calibration.  On a simple LED this will
// just toggle the LED on/off.
void hwLedFlashBlue(uint8_t times);

// Log a message to the host.  On Arduino this writes to Serial.
void hwLog(const char *msg);

// Delay for a number of milliseconds.  Provided here to allow unit
// testing or stubbing on other platforms.
void hwDelayMs(uint32_t ms);

// Read and write 16‑bit values from EEPROM.  These wrappers hide
// platform differences (e.g. AVR vs STM32 EEPROM or flash storage).
uint16_t hwEepromReadU16(uint16_t addr);
void hwEepromWriteU16(uint16_t addr, uint16_t value);
// Erase the entire EEPROM (fills every byte with 0xFF).
void hwEepromErase(void);

// Return the current state of the specified endstop.  Pass 5 or 6
// corresponding to the traction loss actuators.  The return value
// will be true when the endstop is triggered.  The inversion is
// handled automatically based on the configuration supplied to
// hwInitCalibrationPins().
bool hwReadEndstop(uint8_t motor);
bool hwReadEndstopStable(uint8_t motor, uint16_t blank_us, uint8_t samples, uint8_t need_on);

void hwRelayInit();

void hwRelayOn();


void hwRelayOff();

// Overload to log Flash strings without copying to SRAM
void hwLog(const __FlashStringHelper* msg);


void hwInit(void);
void hwInitCalibrationPins(void);
void hwLog(const char* msg);
void hwDelayMs(uint32_t ms);

// LED API existante
void hwLedInit(void);
void hwLedSet(const char* color);
void hwLedFlashBlue(uint8_t times);

// NEW: tick LED à 1 ms (pour blink/glow)
void hwLedTick1ms(void);

