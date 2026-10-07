#ifndef STATUS_LED_H
#define STATUS_LED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void StatusLed_Init(void);
void StatusLed_ShowRgb(uint8_t r, uint8_t g, uint8_t b);

/**
 * Light individual LEDs based on a bitmask (bit 0 = M1 ... bit 6 = M7).
 * LEDs whose bit is clear are turned off.
 * If mask == 0, all 7 LEDs are lit (fallback: no motor ready).
 */
void StatusLed_ShowPerMotor(uint8_t mask, uint8_t r, uint8_t g, uint8_t b);

/**
 * Two-tone display: LEDs whose bit is set in mask get color1 (r1,g1,b1),
 * the others get color0 (r0,g0,b0). Used for per-motor calibration
 * progress (done = red, pending = orange).
 */
void StatusLed_ShowTwoTone(uint8_t mask,
                           uint8_t r1, uint8_t g1, uint8_t b1,
                           uint8_t r0, uint8_t g0, uint8_t b0);

/**
 * Dual-mask display: LEDs in mask1 get color1, LEDs in mask0 (and not in
 * mask1) get color0, all other LEDs are OFF.
 */
void StatusLed_ShowDual(uint8_t mask1, uint8_t r1, uint8_t g1, uint8_t b1,
                        uint8_t mask0, uint8_t r0, uint8_t g0, uint8_t b0);

#ifdef __cplusplus
}
#endif

#endif /* STATUS_LED_H */
