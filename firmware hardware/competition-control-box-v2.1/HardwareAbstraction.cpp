// Code version : 1.0
#include "HardwareAbstraction.h"
#include <string.h>

extern bool hostConnected;

/* ──────────────────────────────────────────────
 *                ARDUINO LEONARDO
 * ────────────────────────────────────────────── */
#if defined(TARGET_LEONARDO)

#include <Arduino.h>
#include <EEPROM.h>
#include "Globals.h"

#if BOX_VERSION == 3
  #define LED_COUNT 7
#else
  #define LED_COUNT 1
#endif

#if BOX_VERSION == 1
  /* Single LED wired to A5 on Box V1 hardware */
  #define ConnectLed A5
#elif BOX_VERSION == 2 || BOX_VERSION == 3
  #include "src/PololuLedStrip.h"
  static PololuLedStrip<A5> ledStrip;
  static rgb_color colors[LED_COUNT];
  enum LedAnimMode : uint8_t { LED_ANIM_STATIC = 0, LED_ANIM_GREEN_GLOW };
  static LedAnimMode ledAnimMode = LED_ANIM_STATIC;
  static unsigned long lastGlowUpdateMs = 0;

  static inline void writeSolidColor(uint8_t r, uint8_t g, uint8_t b) {
    for (uint8_t i = 0; i < LED_COUNT; i++) {
      colors[i] = rgb_color(r, g, b);
    }
    ledStrip.write(colors, LED_COUNT);
  }

  static void writeGlowFrame(unsigned long nowMs, bool redChannel) {
    const unsigned long phase = nowMs % 2400UL;
    uint16_t level;
    if (phase < 1200UL) {
      level = (uint16_t)((phase * 255UL) / 1200UL);
    } else {
      level = (uint16_t)(255UL - (((phase - 1200UL) * 255UL) / 1200UL));
    }
    if (redChannel) {
      writeSolidColor((uint8_t)level, 0, 0);
    } else {
      writeSolidColor(0, (uint8_t)level, 0);
    }
  }
#endif

// -------- Relay (Leonardo) --------
#ifndef RELAY_PIN
  #define RELAY_PIN A0
#endif
#ifndef RELAY_ACTIVE_LOW
  #define RELAY_ACTIVE_LOW 1
#endif

void hwRelayInit(void) {
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
}
void hwRelayOn(void)  { digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? LOW  : HIGH); }
void hwRelayOff(void) { digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW); }

void hwLedInit(void) {
#if BOX_VERSION == 1
  pinMode(ConnectLed, OUTPUT);
  digitalWrite(ConnectLed, LOW);
#elif BOX_VERSION == 2 || BOX_VERSION == 3
  ledAnimMode = LED_ANIM_STATIC;
#endif
  hwLedSet("green_glow");
}

void hwLedSet(const char* color) {
#if BOX_VERSION == 1
  if      (!strcmp(color,"red"))    digitalWrite(ConnectLed, HIGH);
  else if (!strcmp(color,"red_glow")) digitalWrite(ConnectLed, HIGH); // no PWM glow on single LED hardware
  else if (!strcmp(color,"purple")) digitalWrite(ConnectLed, HIGH); // no RGB: use red as mismatch indicator
  else /* green/blue/etc */         digitalWrite(ConnectLed, LOW);
#elif BOX_VERSION == 2 || BOX_VERSION == 3
  if      (!strcmp(color,"green_glow")) {
    ledAnimMode = LED_ANIM_GREEN_GLOW;
    lastGlowUpdateMs = millis();
    writeGlowFrame(lastGlowUpdateMs, false);
    return;
  }
  ledAnimMode = LED_ANIM_STATIC;
  if      (!strcmp(color,"red"))    writeSolidColor(255, 0,   0);
  else if (!strcmp(color,"green"))  writeSolidColor(0,   255, 0);
  else if (!strcmp(color,"orange")) writeSolidColor(255, 80,  0);
  else if (!strcmp(color,"blue"))   writeSolidColor(0,   0,   255);
  else if (!strcmp(color,"purple")) writeSolidColor(160, 0,   200); // motor count mismatch
  else                               writeSolidColor(255, 255, 255);
#endif
}

void hwLedFlashBlue(uint8_t times) {
#if BOX_VERSION == 2 || BOX_VERSION == 3
  for (uint8_t i=0;i<times;i++){
    writeSolidColor(0,0,255); delay(100);
    writeSolidColor(0,0,0);   delay(100);
  }
#else
  for (uint8_t i=0;i<times;i++){
    digitalWrite(ConnectLed, HIGH); delay(100);
    digitalWrite(ConnectLed, LOW);  delay(100);
  }
#endif
}

#define M5CalibrationPin 13
#define M6CalibrationPin A1

static inline void hwDelayUs(uint16_t us) {
#if defined(ARDUINO_ARCH_STM32) || defined(ARDUINO_ARCH_AVR)
  delayMicroseconds(us);
#else
  // fallback : convertit en millisecondes
  if (us < 1000) delayMicroseconds(us);
  else hwDelayMs(us / 1000);
#endif
}

void hwInitCalibrationPins(void) {
  pinMode(M5CalibrationPin, INPUT_PULLUP);
  pinMode(M6CalibrationPin, INPUT_PULLUP);
}

// Soft-only mode: keep a standard baud rate and always allow logs.
// On Leonardo USB CDC this setting is nominal, but keeping it stable avoids confusion.
void hwInit(void) { Serial.begin(115200); }
void hwLog(const char* msg) { Serial.println(msg); }
void hwDelayMs(uint32_t ms) { delay(ms); }

uint16_t hwEepromReadU16(uint16_t addr){ uint16_t v; EEPROM.get(addr, v); return v; }
void     hwEepromWriteU16(uint16_t addr, uint16_t v){ EEPROM.put(addr, v); }
void     hwEepromErase(void){ for (int i = 0; i < (int)EEPROM.length(); i++) EEPROM.update(i, 0xFF); }

bool hwReadEndstop(uint8_t motor) {
  if (motor==5) return ((PINC & (1<<7))==0);
  if (motor==6) return ((PINF & (1<<6))==0);
  return false;
}

bool hwReadEndstopStable(uint8_t motor, uint16_t blank_us, uint8_t samples, uint8_t need_on) {
  if (blank_us) delayMicroseconds(blank_us);   // blanking après commutation
  uint8_t on = 0;
  for (uint8_t i = 0; i < samples; ++i) {
    if (hwReadEndstop(motor)) ++on;
    delayMicroseconds(50);                     // léger spacing entre samples
  }
  return (on >= need_on);
}

// Compat Leonardo : surcharges Flash strings uniquement côté AVR
void hwLog(const __FlashStringHelper* msg) {
  Serial.println(msg);
}

void hwLedTick1ms(void) {
#if BOX_VERSION == 2
  if (ledAnimMode == LED_ANIM_STATIC) return;

  const unsigned long now = millis();
  if (now - lastGlowUpdateMs < 20UL) return;
  lastGlowUpdateMs = now;
  writeGlowFrame(now, false);
#endif
}

// Stubs (gérés ailleurs dans ton code)
void hwDirectionUpdate(void) {}
void hwSingleStep(void) {}



/* ──────────────────────────────────────────────
 *                    STM32
 * ────────────────────────────────────────────── */
#elif defined(TARGET_STM32) || defined(TARGET_STM32F103)

#include "main.h"
extern UART_HandleTypeDef huart2; // configuré dans CubeMX

// === Endstops (si utilisés) ===
#define M5CalibrationPin   GPIO_PIN_7
#define M5CalibrationPort  GPIOC
#define M6CalibrationPin   GPIO_PIN_6
#define M6CalibrationPort  GPIOF

void hwInit(void) {
  // UART/GPIO init: généré par CubeMX ; ici on prépare la LED (PB11)
  __HAL_RCC_GPIOB_CLK_ENABLE();
  GPIO_InitTypeDef gi = {0};
  gi.Pin   = GPIO_PIN_11;
  gi.Mode  = GPIO_MODE_OUTPUT_PP;
  gi.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &gi);
}

void hwInitCalibrationPins(void) {
  // Si besoin, config GPIO via CubeMX
}

void hwLog(const char* msg) {
  HAL_UART_Transmit(&huart2, (uint8_t*)msg, strlen(msg), 100);
  static const char nl[2]="\r\n";
  HAL_UART_Transmit(&huart2, (uint8_t*)nl, 2, 100);
}
void hwDelayMs(uint32_t ms){ HAL_Delay(ms); }

uint16_t hwEepromReadU16(uint16_t addr) {  // si tu as la lib ST EE, sinon remplace par ta NVM
  return EE_ReadVariable(addr);
}
void hwEepromWriteU16(uint16_t addr, uint16_t v) {
  EE_WriteVariable(addr, v);
}
void hwEepromErase(void) {
  // STM32: erase NVM page then reinitialise the EE emulation layer
  EE_Format();
}
bool hwReadEndstop(uint8_t motor) {
  if (motor==5) return HAL_GPIO_ReadPin(M5CalibrationPort, M5CalibrationPin)==GPIO_PIN_RESET;
  if (motor==6) return HAL_GPIO_ReadPin(M6CalibrationPort, M6CalibrationPin)==GPIO_PIN_RESET;
  return false;
}

// -------- Relay (STM32) --------
#ifndef RELAY_PIN
  #define RELAY_PIN  GPIO_PIN_0
#endif
#ifndef RELAY_PORT
  #define RELAY_PORT GPIOA
#endif
#ifndef RELAY_ACTIVE_LOW
  #define RELAY_ACTIVE_LOW 1
#endif

void hwRelayInit(void) { // suppose GPIO configuré par CubeMX
  // Assure l'état OFF
  HAL_GPIO_WritePin(RELAY_PORT, RELAY_PIN, RELAY_ACTIVE_LOW ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
void hwRelayOn(void)  { HAL_GPIO_WritePin(RELAY_PORT, RELAY_PIN, RELAY_ACTIVE_LOW ? GPIO_PIN_RESET : GPIO_PIN_SET); }
void hwRelayOff(void) { HAL_GPIO_WritePin(RELAY_PORT, RELAY_PIN, RELAY_ACTIVE_LOW ? GPIO_PIN_SET   : GPIO_PIN_RESET); }

// -------- LED unique sur PB11, mapping couleurs =====
// green = OFF, red = ON, orange = BLINK 2 Hz, blue = GLOW (respiration)

#ifndef LED_PORT
  #define LED_PORT GPIOB
#endif
#ifndef LED_PIN
  #define LED_PIN  GPIO_PIN_11
#endif
#ifndef LED_ACTIVE_HIGH
  #define LED_ACTIVE_HIGH 1
#endif

static inline void LED_WRITE(int on) {
  HAL_GPIO_WritePin(
    LED_PORT, LED_PIN,
    ((LED_ACTIVE_HIGH ? on : !on) ? GPIO_PIN_SET : GPIO_PIN_RESET)
  );
}

typedef enum { LEDM_OFF=0, LEDM_ON, LEDM_BLINK, LEDM_GLOW } LedMode;
static volatile LedMode ledMode = LEDM_OFF;

/* Soft-PWM 62.5 Hz: période 16 ms en tick 1 ms, 17 niveaux (0..16) */
#define LED_PWM_PERIOD_TICKS  16u
static volatile uint8_t  pwmCounter = 0;
static volatile uint8_t  duty       = 0;

/* Blink 2 Hz */
#define LED_BLINK_HALF_MS     250u
static volatile uint32_t tBlink = 0;
static volatile uint8_t  blinkOn = 0;

/* Glow : variation duty toutes 60 ms 0..16..0 */
#define LED_GLOW_STEP_MS      60u
static volatile uint32_t tGlow = 0;
static volatile int8_t   glowStep = +1;

void hwLedInit(void) {
  LED_WRITE(0);
  ledMode = LEDM_OFF;
  hwLedSet("green");
}

void hwLedSet(const char* color) {
  if (!color) return;

  if (!strcmp(color,"green")) {            // OFF
    ledMode = LEDM_OFF;
    duty = 0; LED_WRITE(0);
  } else if (!strcmp(color,"red")) {       // ON
    ledMode = LEDM_ON;
    duty = LED_PWM_PERIOD_TICKS; LED_WRITE(1);
  } else if (!strcmp(color,"orange")) {    // BLINK
    ledMode = LEDM_BLINK;
    blinkOn = 0; duty = 0; tBlink = HAL_GetTick(); LED_WRITE(0);
  } else if (!strcmp(color,"blue")) {      // GLOW
    ledMode = LEDM_GLOW;
    duty = 0; glowStep = +1; tGlow = HAL_GetTick();
  } else {                                 // défaut = ON
    ledMode = LEDM_ON;
    duty = LED_PWM_PERIOD_TICKS; LED_WRITE(1);
  }
}

void hwLedFlashBlue(uint8_t times) {
  for (uint8_t i=0;i<times;i++){
    LED_WRITE(1); HAL_Delay(100);
    LED_WRITE(0); HAL_Delay(100);
  }
}

/* Tick 1 ms pour PWM/blink/glow — À appeler chaque ms (SysTick) */
void hwLedTick1ms(void) {
  // PWM ~62.5 Hz
  if (++pwmCounter >= LED_PWM_PERIOD_TICKS) pwmCounter = 0;
  LED_WRITE(pwmCounter < duty);

  uint32_t now = HAL_GetTick();

  if (ledMode == LEDM_BLINK) {
    if ((now - tBlink) >= LED_BLINK_HALF_MS) {
      tBlink = now; blinkOn ^= 1;
      duty = blinkOn ? LED_PWM_PERIOD_TICKS : 0;
    }
  }
  if (ledMode == LEDM_GLOW) {
    if ((now - tGlow) >= LED_GLOW_STEP_MS) {
      tGlow = now;
      int16_t d = (int16_t)duty + glowStep;
      if (d >= (int16_t)LED_PWM_PERIOD_TICKS) { d = LED_PWM_PERIOD_TICKS; glowStep = -glowStep; }
      if (d <= 0)                              { d = 0;                      glowStep = -glowStep; }
      duty = (uint8_t)d;
    }
  }
}

// Stubs si tes moteurs sont gérés ailleurs
void hwDirectionUpdate(void) {}
void hwSingleStep(void) {}

#else
  #error "Cible non reconnue. Merci de sélectionner Leonardo"
#endif
