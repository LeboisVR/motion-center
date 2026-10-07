/**
 ******************************************************************************
 * @file    debug_uart.h
 * @brief   Debug UART helper shared by firmware modules.
 ******************************************************************************
 */
#ifndef DEBUG_UART_H
#define DEBUG_UART_H

#ifdef __cplusplus
extern "C" {
#endif

void DebugUart_Print(const char *s);
void DebugUart_Printf(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* DEBUG_UART_H */