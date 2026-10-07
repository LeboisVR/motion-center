/**
 ******************************************************************************
 * @file    debug_uart.c
 * @brief   UART2 debug logger.
 ******************************************************************************
 */
#include "debug_uart.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "main.h"

extern UART_HandleTypeDef huart2;

void DebugUart_Print(const char *s)
{
  if (s == NULL) {
    return;
  }
  size_t len = strlen(s);
  if (len == 0u) {
    return;
  }
  HAL_UART_Transmit(&huart2, (uint8_t *)s, (uint16_t)len, 20u);
}

void DebugUart_Printf(const char *fmt, ...)
{
  char buf[256];
  va_list ap;

  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  if (n <= 0) {
    return;
  }
  if (n > (int)sizeof(buf)) {
    n = (int)sizeof(buf);
  }

  HAL_UART_Transmit(&huart2, (uint8_t *)buf, (uint16_t)n, 20u);
}