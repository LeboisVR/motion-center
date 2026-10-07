#pragma once
// ================================================================
//  Debug — coût zéro en release (tous les blocs éliminés par le préprocesseur)
//
//  Activation : build_flags = -DDEBUG dans platformio.ini (env debug)
//  Output     : Serial1 (UART matériel, pins 0/1 sur Leonardo → adaptateur FTDI)
//               STM32 : remplacer Serial1 par USARTx dans hwDebugInit()
//
//  Macros disponibles :
//    DBG(msg)            — message flash-safe (F() implicite)
//    DBG_VAL(label, v)   — label + valeur entière
//    DBG_STATE(label, v) — label + état (uint8_t affiché en décimal)
// ================================================================

#ifdef DEBUG

  #ifndef DEBUG_BAUD
    #define DEBUG_BAUD 115200UL
  #endif

  // Initialisation — à appeler une fois dans setup()
  static inline void hwDebugInit() { Serial1.begin(DEBUG_BAUD); }

  // Message seul (string dans flash)
  #define DBG(msg)              Serial1.println(F(msg))

  // label (flash) + valeur entière
  #define DBG_VAL(label, v)     do { Serial1.print(F(label)); Serial1.println((long)(v)); } while(0)

  // label (flash) + état FSM (uint8_t)
  #define DBG_STATE(label, v)   do { Serial1.print(F(label)); Serial1.println((uint8_t)(v)); } while(0)

  // Changement de paramètre : label + ancienne valeur → nouvelle valeur
  #define DBG_CHANGE(label, old_v, new_v) \
    do { Serial1.print(F(label)); \
         Serial1.print((long)(old_v)); Serial1.print(F("->")); \
         Serial1.println((long)(new_v)); } while(0)

#else

  // Release : tout est éliminé — zéro octet de flash
  static inline void hwDebugInit() {}
  #define DBG(msg)                      do {} while(0)
  #define DBG_VAL(label, v)             do {} while(0)
  #define DBG_STATE(label, v)           do {} while(0)
  #define DBG_CHANGE(label, old_v, new_v) do {} while(0)

#endif // DEBUG
