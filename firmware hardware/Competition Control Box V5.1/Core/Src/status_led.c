#include "status_led.h"

#include "main.h"
#include "core_cm4.h"

#define WS2812_LED_COUNT 7U
#define STATUS_LED_BRIGHTNESS_PCT 45U

static uint8_t scale_channel(uint8_t v)
{
  uint16_t scaled = (uint16_t)v * (uint16_t)STATUS_LED_BRIGHTNESS_PCT;
  return (uint8_t)(scaled / 100U);
}

static uint32_t ws2812_ns_to_cycles(uint32_t ns)
{
  uint64_t num = (uint64_t)SystemCoreClock * (uint64_t)ns;
  return (uint32_t)((num + 999999999ULL) / 1000000000ULL);
}

static void ws2812_delay_cycles(uint32_t cycles)
{
  uint32_t start = DWT->CYCCNT;
  while ((uint32_t)(DWT->CYCCNT - start) < cycles) {
  }
}

static void ws2812_send_grb(uint8_t g, uint8_t r, uint8_t b)
{
  const uint8_t bytes[3] = { g, r, b };
  const uint32_t t0h = ws2812_ns_to_cycles(350U);
  const uint32_t t0l = ws2812_ns_to_cycles(800U);
  const uint32_t t1h = ws2812_ns_to_cycles(700U);
  const uint32_t t1l = ws2812_ns_to_cycles(600U);

  for (uint32_t bi = 0; bi < 3U; bi++) {
    uint8_t v = bytes[bi];
    for (uint8_t bit = 0; bit < 8U; bit++) {
      if (v & 0x80U) {
        HAL_GPIO_WritePin(WS2812_GPIO_Port, WS2812_Pin, GPIO_PIN_SET);
        ws2812_delay_cycles(t1h);
        HAL_GPIO_WritePin(WS2812_GPIO_Port, WS2812_Pin, GPIO_PIN_RESET);
        ws2812_delay_cycles(t1l);
      } else {
        HAL_GPIO_WritePin(WS2812_GPIO_Port, WS2812_Pin, GPIO_PIN_SET);
        ws2812_delay_cycles(t0h);
        HAL_GPIO_WritePin(WS2812_GPIO_Port, WS2812_Pin, GPIO_PIN_RESET);
        ws2812_delay_cycles(t0l);
      }
      v <<= 1;
    }
  }
}

/* Trame complete (7 LEDs, GRB). Transmission DOUBLE : au premier envoi le
   chemin de code est "froid" (latence flash/cache) et les tout premiers bits
   peuvent etre deformes — seule la LED 1 les recoit directement (les
   suivantes lisent le signal regenere par la LED 1), d'ou sa couleur
   legerement decalee. Le second envoi, chemin chaud, la corrige. */
static void ws2812_show_frame(const uint8_t frame[WS2812_LED_COUNT][3])
{
  const uint32_t latch = ws2812_ns_to_cycles(80000U);
  for (uint32_t pass = 0; pass < 2U; pass++) {
    __disable_irq();
    for (uint32_t i = 0; i < WS2812_LED_COUNT; i++) {
      ws2812_send_grb(frame[i][0], frame[i][1], frame[i][2]);
    }
    __enable_irq();
    ws2812_delay_cycles(latch);   /* reset >80 µs entre les deux trames */
  }
}

void StatusLed_Init(void)
{
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0U;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

void StatusLed_ShowRgb(uint8_t r, uint8_t g, uint8_t b)
{
  uint8_t rs = scale_channel(r);
  uint8_t gs = scale_channel(g);
  uint8_t bs = scale_channel(b);

  uint8_t frame[WS2812_LED_COUNT][3];
  for (uint32_t i = 0; i < WS2812_LED_COUNT; i++) {
    frame[i][0] = gs; frame[i][1] = rs; frame[i][2] = bs;
  }
  ws2812_show_frame(frame);
}

void StatusLed_ShowPerMotor(uint8_t mask, uint8_t r, uint8_t g, uint8_t b)
{
  /* If no motor is ready, fall back to lighting all LEDs. */
  if (mask == 0u) {
    mask = (uint8_t)((1u << WS2812_LED_COUNT) - 1u);
  }

  uint8_t rs = scale_channel(r);
  uint8_t gs = scale_channel(g);
  uint8_t bs = scale_channel(b);

  uint8_t frame[WS2812_LED_COUNT][3];
  for (uint32_t i = 0; i < WS2812_LED_COUNT; i++) {
    if (mask & (uint8_t)(1u << i)) {
      frame[i][0] = gs; frame[i][1] = rs; frame[i][2] = bs;
    } else {
      frame[i][0] = 0u; frame[i][1] = 0u; frame[i][2] = 0u;
    }
  }
  ws2812_show_frame(frame);
}

void StatusLed_ShowTwoTone(uint8_t mask,
                           uint8_t r1, uint8_t g1, uint8_t b1,
                           uint8_t r0, uint8_t g0, uint8_t b0)
{
  uint8_t rs1 = scale_channel(r1);
  uint8_t gs1 = scale_channel(g1);
  uint8_t bs1 = scale_channel(b1);
  uint8_t rs0 = scale_channel(r0);
  uint8_t gs0 = scale_channel(g0);
  uint8_t bs0 = scale_channel(b0);

  uint8_t frame[WS2812_LED_COUNT][3];
  for (uint32_t i = 0; i < WS2812_LED_COUNT; i++) {
    if (mask & (uint8_t)(1u << i)) {
      frame[i][0] = gs1; frame[i][1] = rs1; frame[i][2] = bs1;
    } else {
      frame[i][0] = gs0; frame[i][1] = rs0; frame[i][2] = bs0;
    }
  }
  ws2812_show_frame(frame);
}

void StatusLed_ShowDual(uint8_t mask1, uint8_t r1, uint8_t g1, uint8_t b1,
                        uint8_t mask0, uint8_t r0, uint8_t g0, uint8_t b0)
{
  uint8_t rs1 = scale_channel(r1);
  uint8_t gs1 = scale_channel(g1);
  uint8_t bs1 = scale_channel(b1);
  uint8_t rs0 = scale_channel(r0);
  uint8_t gs0 = scale_channel(g0);
  uint8_t bs0 = scale_channel(b0);

  uint8_t frame[WS2812_LED_COUNT][3];
  for (uint32_t i = 0; i < WS2812_LED_COUNT; i++) {
    uint8_t bit = (uint8_t)(1u << i);
    if (mask1 & bit) {
      frame[i][0] = gs1; frame[i][1] = rs1; frame[i][2] = bs1;
    } else if (mask0 & bit) {
      frame[i][0] = gs0; frame[i][1] = rs0; frame[i][2] = bs0;
    } else {
      frame[i][0] = 0u; frame[i][1] = 0u; frame[i][2] = 0u;
    }
  }
  ws2812_show_frame(frame);
}
