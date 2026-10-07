/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : SRT80 PRO Control Box V2.3 — STM32G431CBT6
  *                   Porte depuis la PRO V2 (STM32F103). 6 moteurs step/dir,
  *                   endstop par moteur, homing/calibration sur M1..M6,
  *                   USB CDC (SimHub + Motion Center), bootloader IAP dual-bank.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usb_device.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdbool.h>
#include <string.h>
#include "usbd_core.h"
#include "usbd_desc.h"
#include "usbd_cdc.h"
#include "usbd_cdc_if.h"
#include <stdio.h>          /* snprintf */
#include "Actuator.h"       /* ax[], axCfg[], boxCfg, axStep, axMicros */
#include "Homing.h"         /* homing MIN + calibration */
#include "HomingDir.h"      /* direction de homing MIN/MAX (commune) */
#include "PosMap.h"         /* mapping commun consigne 15 bits -> cible */
#include "PFrame.h"         /* format commun trame 'P' (7 axes, 15 octets) */
#include "Nvm.h"            /* persistance flash (max/margin/homing sps) */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define NUM_MOTORS      6   /* M1..M6, tous steppers avec endstop */
_Static_assert(NUM_MOTORS <= MAX_ACTUATORS,
               "ax[] trop petit : augmentez MAX_ACTUATORS (build flag).");
#define SERVO_SETTLE_MS 200u
/* Delai apres fermeture du relais avant d'annoncer CALIBRATED, le temps que
   les drives A6-RS s'initialisent (enableServo attend deja 2 s relais). */
#define DRIVER_INIT_MS  3000u
#define STASH_SIZE      256u
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */
/* hUsbDeviceFS est defini dans USB_Device/App/usb_device.c */
extern USBD_HandleTypeDef hUsbDeviceFS;

/* USB receive stash – rempli par CDC_Receive_FS (usbd_cdc_if.c) */
uint8_t  s_stash[STASH_SIZE];
volatile uint16_t s_len;

/* System flags */
bool     simhubConnected;
volatile bool     servoEnabled;
volatile uint32_t servoMotionAllowedAt;
static bool s_pendingDisable = false;  /* SC recu, attente retour a 0 avant disable */
static uint16_t s_lastStepUs[NUM_MOTORS] = {0};
static int8_t   s_dirV[NUM_MOTORS]       = {0, 0, 0, 0, 0, 0};

/* Requis par Homing.c (sur l'AVR : Globals.cpp). */
bool hostConnected = false;

/* Reponse differee des commandes de calibration (contrat OK/ERR AVR :
   la reponse part a la FIN de l'operation, pas au lancement). */
typedef enum { CALOP_NONE = 0, CALOP_DETECT_MIN, CALOP_CALIB } CalOp;
static CalOp   s_calOp     = CALOP_NONE;
static uint8_t s_calMask   = 0;          /* bits = idx ax[] concernes */

/* Tables d'adresses NVM indexees par moteur (0..5). */
static const uint16_t NVM_MAX[NUM_MOTORS] = {
  EEPROM_M1MAX_ADDR, EEPROM_M2MAX_ADDR, EEPROM_M3MAX_ADDR,
  EEPROM_M4MAX_ADDR, EEPROM_M5MAX_ADDR, EEPROM_M6MAX_ADDR };
static const uint16_t NVM_MARGIN[NUM_MOTORS] = {
  EEPROM_M1MARGIN_ADDR, EEPROM_M2MARGIN_ADDR, EEPROM_M3MARGIN_ADDR,
  EEPROM_M4MARGIN_ADDR, EEPROM_M5MARGIN_ADDR, EEPROM_M6MARGIN_ADDR };
static const uint16_t NVM_CONN[NUM_MOTORS] = {
  EEPROM_M1CONNECTED_ADDR, EEPROM_M2CONNECTED_ADDR, EEPROM_M3CONNECTED_ADDR,
  EEPROM_M4CONNECTED_ADDR, EEPROM_M5CONNECTED_ADDR, EEPROM_M6CONNECTED_ADDR };
static const uint16_t NVM_HDIR[NUM_MOTORS] = {
  EEPROM_M1HOMINGDIR_ADDR, EEPROM_M2HOMINGDIR_ADDR, EEPROM_M3HOMINGDIR_ADDR,
  EEPROM_M4HOMINGDIR_ADDR, EEPROM_M5HOMINGDIR_ADDR, EEPROM_M6HOMINGDIR_ADDR };
static const uint16_t NVM_EPARK[NUM_MOTORS] = {
  EEPROM_M1ENDPARK_ADDR, EEPROM_M2ENDPARK_ADDR, EEPROM_M3ENDPARK_ADDR,
  EEPROM_M4ENDPARK_ADDR, EEPROM_M5ENDPARK_ADDR, EEPROM_M6ENDPARK_ADDR };
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
/* USER CODE BEGIN PFP */
static void MX_TIM2_Init(void);
static void MX_CRS_Init(void);
static void delay_us(uint16_t us);
static void stepDirServiceFastV2(void);
static void cdc_send_str(const char *s);
static void handleTextCommand(const char *line);
static void sendStatusLine(void);
static void enterDFU(void);
static void actuatorConfigInit(void);
static void serviceCalibReply(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* ---------- Stash helpers (called from main loop only) -------------------- */
static inline void stash_consume(uint16_t n)
{
  __disable_irq();
  if (n >= s_len) { s_len = 0; }
  else { memmove(s_stash, s_stash + n, s_len - n); s_len -= n; }
  __enable_irq();
}

static inline uint16_t be16(const uint8_t *p)
{
  return (uint16_t)((p[0] << 8) | p[1]);
}

/* ---------- Tables broches STEP / DIR (multi-port A et B) -----------------
 *  M1,M2,M5,M6 -> GPIOB   |   M3,M4 -> GPIOA
 *  Le stepping construit un masque par port a partir de ces tables. */
static GPIO_TypeDef * const STEP_PORT[NUM_MOTORS] = {
  S1_GPIO_Port, S2_GPIO_Port, S3_GPIO_Port, S4_GPIO_Port, S5_GPIO_Port, S6_GPIO_Port };
static const uint16_t       STEP_PIN [NUM_MOTORS] = {
  S1_Pin, S2_Pin, S3_Pin, S4_Pin, S5_Pin, S6_Pin };
static GPIO_TypeDef * const DIR_PORT [NUM_MOTORS] = {
  D1_GPIO_Port, D2_GPIO_Port, D3_GPIO_Port, D4_GPIO_Port, D5_GPIO_Port, D6_GPIO_Port };
static const uint16_t       DIR_PIN  [NUM_MOTORS] = {
  D1_Pin, D2_Pin, D3_Pin, D4_Pin, D5_Pin, D6_Pin };

/* Endstops (un par moteur), index 0..5 = M1..M6. */
static GPIO_TypeDef * const ES_PORT[NUM_MOTORS] = {
  M1ENDSTOP_GPIO_Port, M2ENDSTOP_GPIO_Port, M3ENDSTOP_GPIO_Port,
  M4ENDSTOP_GPIO_Port, M5ENDSTOP_GPIO_Port, M6ENDSTOP_GPIO_Port };
static const uint16_t       ES_PIN [NUM_MOTORS] = {
  M1ENDSTOP_Pin, M2ENDSTOP_Pin, M3ENDSTOP_Pin,
  M4ENDSTOP_Pin, M5ENDSTOP_Pin, M6ENDSTOP_Pin };

/* -------------------------------------------------------------------------- */
void enableServo(){
  HAL_GPIO_WritePin(ENABLE_RELAY_GPIO_Port, ENABLE_RELAY_Pin, GPIO_PIN_SET);   /* relais ON (actif haut) – PB11 */
  HAL_Delay(2000);                                                              /* attendre que le relais soit stable */
  servoEnabled = true;
  servoMotionAllowedAt = HAL_GetTick() + SERVO_SETTLE_MS;
}

void disableServo(){
  HAL_GPIO_WritePin(ENABLE_RELAY_GPIO_Port, ENABLE_RELAY_Pin, GPIO_PIN_RESET); /* relais OFF – drivers hors tension */
  servoEnabled = false;
  servoMotionAllowedAt = 0;
  /* Reset tracking : la position physique d'arret devient le nouveau zero. */
  for (uint8_t i = 0; i < NUM_MOTORS; i++) {
    ax[i].pos    = 0U;
    ax[i].target = 0U;
  }
}

/* ==========================================================================
 *  Endstops M1..M6 + hooks plateforme requis par Homing.c
 *  (memes signatures que HardwareAbstraction AVR)
 * ========================================================================== */

/* Lecture brute : switch actif bas (vers GND, pull-up materielle/interne).
   'motor' est 1-based (M1..M6), comme passe par Homing.c. */
static bool hwReadEndstop(uint8_t motor)
{
  if (motor < 1U || motor > NUM_MOTORS) return false;
  const uint8_t i = (uint8_t)(motor - 1U);
  return HAL_GPIO_ReadPin(ES_PORT[i], ES_PIN[i]) == GPIO_PIN_RESET;
}

/* Lecture filtree : blanking + vote majoritaire (copie du contrat AVR). */
bool hwReadEndstopStable(uint8_t motor, uint16_t blank_us, uint8_t samples, uint8_t need_on)
{
  if (blank_us) delay_us(blank_us);
  uint8_t on = 0;
  for (uint8_t i = 0; i < samples; ++i) {
    if (hwReadEndstop(motor)) ++on;
    delay_us(50U);
  }
  return (on >= need_on);
}

/* Pas de LED sur cette box : no-op. */
void hwLedSet(const char* color) { (void)color; }
void refreshConnectionLed(void)  { }

/* Logs homing/calibration -> Motion Center via USB CDC. */
void hwLog(const char* msg)
{
  cdc_send_str(msg);
  cdc_send_str("\n");
}

void SerialReaderP(void)
{
  /* Suivi d'inactivite : SimHub n'envoie aucun terminateur de ligne. */
  static uint16_t s_prevLen    = 0U;
  static uint32_t s_lastRxTick = 0U;
  if (s_len != s_prevLen) { s_prevLen = s_len; s_lastRxTick = HAL_GetTick(); }

  for (;;) {
    if (s_len == 0U) break;

    uint8_t c0 = s_stash[0];

    /* ===== Trames de cibles SimHub : format commun shared/PFrame.h ======
       'P' + 7 axes x 2 octets BE (15 bits). Cette box exploite les axes
       1-6 ; l'axe 7 est consomme et ignore. */
    if (c0 == PFRAME_TAG) {
      if (s_len >= PFRAME_LEN) {
        const uint8_t *payload = &s_stash[1];
        for (uint8_t i = 0; i < NUM_MOTORS; i++) {
          if (!axCfg[i].connected)            continue;
          if (!ax[i].homed || ax[i].homing)   continue;
          if (calibIsBusy(i))                 continue;
          uint16_t u = PFrame_Axis(payload, i);
          if (axCfg[i].maxPos != 0U) {
            ax[i].target = PosMap_TargetOptional(u, axCfg[i].maxPos,
                                                 axCfg[i].margin);
          } else {
            ax[i].target = u;
          }
        }
        stash_consume(PFRAME_LEN);
        continue;
      }
      /* Trame incomplete : attendre la suite. Si plus rien apres 20 ms,
         c'est peut-etre une commande texte commencant par 'P'. */
      if ((HAL_GetTick() - s_lastRxTick) < 20U) break;
    }

    /* ===== Trames binaires legacy : 'S' + {A,T,C} ======================== */
    if (c0 == 'S') {
      if (s_len < 2U) break;

      uint8_t cmd = s_stash[1];

      if (cmd == 'A') {
        stash_consume(2U);
        for (uint8_t i = 0; i < NUM_MOTORS; i++) {
          ax[i].pos = 0U;
          ax[i].target   = 0U;
          s_lastStepUs[i] = 0U;
          s_dirV[i]       = 0;
        }
        simhubConnected = true;
        enableServo();
        cdc_send_str("OK\n");
        continue;
      }

      if (cmd == 'T') {
        if (s_len < 12U) break;
        bool valid = true;
        for (uint8_t m = 0; m < 5U; m++) {
          if (s_stash[2U + m * 2U] & 0x80U) { valid = false; break; }
        }
        if (!valid) { stash_consume(1U); continue; }
        const uint8_t *payload = &s_stash[2];
        ax[0].target = be16(&payload[0]);
        ax[1].target = be16(&payload[2]);
        ax[2].target = be16(&payload[4]);
        ax[3].target = be16(&payload[6]);
        stash_consume(12U);
        continue;
      }

      if (cmd == 'C') {
        stash_consume(2U);
        simhubConnected = false;
        for (uint8_t i = 0; i < NUM_MOTORS; i++) ax[i].target = 0U;
        s_pendingDisable = true;
        continue;
      }
      /* 'S' + autre chose (SH_*, SET…) : ligne texte, chemin ci-dessous. */
    }

    /* ===== Commandes texte (SimHub handshake / Motion Center) ============ */
    if (c0 < 0x20 || c0 > 0x7E) { stash_consume(1U); continue; }

    uint16_t nl = 0U;
    bool eol = false;
    while (nl < s_len) {
      uint8_t c = s_stash[nl];
      if (c == '\n' || c == '\r') { eol = true; break; }
      nl++;
    }
    if (!eol) {
      /* SimHub n'envoie AUCUN terminateur (SH_CONNECTED, SH_START…).
         Repli : si plus rien depuis 20 ms, traiter le stash comme ligne. */
      if ((HAL_GetTick() - s_lastRxTick) < 20U) {
        if (s_len >= (uint16_t)(STASH_SIZE - 1U)) s_len = 0U;
        break;
      }
      nl = s_len;
    }

    char line[48];
    uint16_t n = nl;
    if (n > (uint16_t)(sizeof(line) - 1U)) n = (uint16_t)(sizeof(line) - 1U);
    memcpy(line, s_stash, n);
    line[n] = '\0';

    uint16_t adv = nl;
    if (eol) {
      adv++;
      if (adv < s_len && (s_stash[adv] == '\n' || s_stash[adv] == '\r')) adv++;
    }
    stash_consume(adv);

    handleTextCommand(line);
  }
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USB_Device_Init();
  /* USER CODE BEGIN 2 */
  MX_CRS_Init();        /* recale HSI48 sur les SOF USB (precision full-speed) */
  MX_TIM2_Init();       /* TIM2 : free-running 1 MHz counter pour delay_us() */
  actuatorConfigInit(); /* peuple ax[]/axCfg[] pour les 6 axes */
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    axMicros();               /* entretient le compteur µs 32 bits (anti-wrap TIM2) */
    SerialReaderP();          /* traite le stash USB (SH_START, ST, etc.) */
    homingTick();             /* FSM homing MIN (non bloquant) */
    calibTick();              /* FSM calibration complete (non bloquant) */
    serviceCalibReply();      /* OK/ERR differe en fin de calibration */

    /* Gel des axes non impliques pendant un homing/calibration, jusqu'a la
       premiere frame P appliquee apres la fin (anti a-coup). */
    {
      bool anyBusy = false;
      for (uint8_t i = 0; i < NUM_MOTORS; i++) {
        if (homingIsActive(i) || calibIsBusy(i)) { anyBusy = true; break; }
      }
      if (anyBusy) {
        for (uint8_t i = 0; i < NUM_MOTORS; i++) {
          if (!(homingIsActive(i) || calibIsBusy(i))) ax[i].target = ax[i].pos;
        }
      }
    }
    stepDirServiceFastV2();   /* genere les pas vers ax[].target */

    /* Disable differe (commande binaire 'SC') : couper les servos une fois
       tous les axes revenus a 0. */
    if (s_pendingDisable) {
      bool allZero = true;
      for (uint8_t i = 0; i < NUM_MOTORS; i++) {
        if (ax[i].pos != 0U) { allZero = false; break; }
      }
      if (allZero) { s_pendingDisable = false; disableServo(); }
    }
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /* Boost la tension coeur pour 170 MHz. */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

  /** HSE 8 MHz -> PLL 170 MHz ; HSI48 pour l'USB (recale par CRS/SOF). */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE | RCC_OSCILLATORTYPE_HSI48;
  RCC_OscInitStruct.HSEState       = RCC_HSE_ON;
  RCC_OscInitStruct.HSI48State     = RCC_HSI48_ON;
  RCC_OscInitStruct.PLL.PLLState   = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource  = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM       = RCC_PLLM_DIV2;   /* 8/2  = 4 MHz  */
  RCC_OscInitStruct.PLL.PLLN       = 85;              /* 4*85 = 340 MHz VCO */
  RCC_OscInitStruct.PLL.PLLP       = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ       = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR       = RCC_PLLR_DIV2;   /* 340/2 = 170 MHz */
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Bus clocks : SYSCLK=170, HCLK=170, PCLK1=170, PCLK2=170. */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                              | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider  = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_4) != HAL_OK)
  {
    Error_Handler();
  }

  /** USB clock = HSI48. */
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_USB;
  PeriphClkInit.UsbClockSelection    = RCC_USBCLKSOURCE_HSI48;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4b */
/* CRS : recale HSI48 sur les SOF USB (precision USB full-speed sans quartz
   dedie). Appele apres l'enumeration ; sans hote USB, HSI48 reste en roue
   libre (~1%), suffisant pour demarrer. */
static void MX_CRS_Init(void)
{
  RCC_CRSInitTypeDef crs = {0};
  __HAL_RCC_CRS_CLK_ENABLE();
  crs.Prescaler             = RCC_CRS_SYNC_DIV1;
  crs.Source                = RCC_CRS_SYNC_SOURCE_USB;
  crs.Polarity              = RCC_CRS_SYNC_POLARITY_RISING;
  crs.ReloadValue           = __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000U, 1000U);
  crs.ErrorLimitValue       = RCC_CRS_ERRORLIMIT_DEFAULT;
  crs.HSI48CalibrationValue = RCC_CRS_HSI48CALIBRATION_DEFAULT;
  HAL_RCCEx_CRSConfig(&crs);
}
/* USER CODE END 4b */

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();

  /* Niveau initial : STEP/DIR bas, relais relache. */
  HAL_GPIO_WritePin(GPIOA, S3_Pin | S4_Pin | D3_Pin | D4_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(GPIOB, S1_Pin | S2_Pin | S5_Pin | S6_Pin
                          | D1_Pin | D2_Pin | D5_Pin | D6_Pin
                          | ENABLE_RELAY_Pin, GPIO_PIN_RESET);

  /* STEP + DIR sur GPIOA (M3, M4). */
  GPIO_InitStruct.Pin   = S3_Pin | S4_Pin | D3_Pin | D4_Pin;
  GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull  = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* STEP + DIR sur GPIOB (M1, M2, M5, M6). */
  GPIO_InitStruct.Pin   = S1_Pin | S2_Pin | S5_Pin | S6_Pin
                        | D1_Pin | D2_Pin | D5_Pin | D6_Pin;
  GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull  = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* Relais d'alimentation drivers (PB11, actif haut). */
  GPIO_InitStruct.Pin   = ENABLE_RELAY_Pin;
  GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull  = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(ENABLE_RELAY_GPIO_Port, &GPIO_InitStruct);

  /* Endstops : entrees pull-up interne (switch actif bas vers GND). */
  GPIO_InitStruct.Pin  = M3ENDSTOP_Pin;                 /* GPIOC : PC13 */
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(M3ENDSTOP_GPIO_Port, &GPIO_InitStruct);

  GPIO_InitStruct.Pin  = M6ENDSTOP_Pin;                 /* GPIOA : PA9  */
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(M6ENDSTOP_GPIO_Port, &GPIO_InitStruct);

  GPIO_InitStruct.Pin  = M1ENDSTOP_Pin | M2ENDSTOP_Pin  /* GPIOB : PB3,6,0,1 */
                       | M4ENDSTOP_Pin | M5ENDSTOP_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /* Entrees reservees IN1/IN2 (PA2/PA3), pull-up (non utilisees). */
  GPIO_InitStruct.Pin  = IN1_Pin | IN2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */
  /* Etat sur au boot : relais relache (LOW). */
  HAL_GPIO_WritePin(ENABLE_RELAY_GPIO_Port, ENABLE_RELAY_Pin, GPIO_PIN_RESET);
  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/**
  * @brief  TIM2 – free-running 1 MHz counter (polled, no IRQ).
  *         SYSCLK 170 MHz, APB1 = HCLK/1 = 170 MHz -> TIM2 clock = 170 MHz.
  *         PSC=169 -> tick = 170 MHz / 170 = 1 MHz (1 µs) exact.
  *         ARR=0xFFFF -> compteur 16 bits, wrap a 65535 µs.
  */
static void MX_TIM2_Init(void)
{
  __HAL_RCC_TIM2_CLK_ENABLE();
  TIM2->PSC  = 169U;      /* 170 MHz / 170 = 1 MHz tick (1 µs) */
  TIM2->ARR  = 0xFFFFFFFFU; /* TIM2 = 32 bits sur G4, on n'exploite que 16 bits utiles */
  TIM2->CNT  = 0U;
  TIM2->EGR  = TIM_EGR_UG;
  TIM2->SR   = 0U;
  TIM2->DIER = 0U;        /* no interrupt */
  TIM2->CR1  = TIM_CR1_CEN;
}

static void delay_us(uint16_t us)
{
  uint16_t start = (uint16_t)TIM2->CNT;
  while ((uint16_t)((uint16_t)TIM2->CNT - start) < us) { }
}

/* Base de temps µs 32 bits pour le module Actuator (axStep / homing).
   TIM2 tourne a 1 µs ; on extrait 16 bits et on etend par suivi du wrap.
   Doit etre appele regulierement (< 65 ms) -> tete de boucle principale. */
static volatile uint32_t s_usHigh = 0U;
static volatile uint16_t s_usLast = 0U;
uint32_t axMicros(void)
{
  uint16_t now = (uint16_t)TIM2->CNT;
  if (now < s_usLast) s_usHigh += 0x10000UL;   /* TIM2 a reboucle (16 bits utiles) */
  s_usLast = now;
  return s_usHigh + now;
}

/* Initialise le modele verin partage : 6 axes step/dir avec endstop.
   M1-M4 connectes par defaut ; M5/M6 optionnels (actives dans Motion Center).
   max/margin/etc charges depuis la NVM si le schema est valide. */
static void actuatorConfigInit(void)
{
  boxCfg.version      = 4U;              /* PRO -> BOX=4 cote Motion Center */
  boxCfg.processor    = PROC_STM32G4;
  boxCfg.maxActuators = NUM_MOTORS;
  boxCfg.estopMode    = ESTOP_HW_ONLY;
  boxCfg.estopAction  = ESTOP_ACTION_DISABLE_SERVOS;

  const bool nvmValid =
      (hwEepromReadU16(EEPROM_HAS_BEEN_FACTORYRESET) == EEPROM_SCHEMA_VERSION);

  for (uint8_t i = 0; i < NUM_MOTORS; i++) {
    const bool base = (i < 4U);          /* M1-M4 cables en dur */
    const uint16_t m  = nvmValid ? hwEepromReadU16(NVM_MAX[i])
                                 : (base ? 32767U : 0U);
    const uint16_t mg = nvmValid ? hwEepromReadU16(NVM_MARGIN[i]) : 0U;
    const bool     cn = nvmValid ? (hwEepromReadU16(NVM_CONN[i]) != 0U) : base;
    const bool     hd = nvmValid ? (hwEepromReadU16(NVM_HDIR[i]) == 1U) : false;
    const uint16_t ep = nvmValid ? hwEepromReadU16(NVM_EPARK[i]) : 0xFFFFU;

    axCfg[i].maxPos          = m;
    axCfg[i].margin          = mg;
    axCfg[i].dir             = (int8_t)+1;
    axCfg[i].connected       = cn;
    axCfg[i].hasEndstop      = true;      /* M1..M6 ENDSTOP cables */
    axCfg[i].hastobehomed    = false;     /* homing a la demande (Motion Center) */
    axCfg[i].absoluteEncoder = false;
    axCfg[i].hometoMax       = hd;
    axCfg[i].endParkPct      = (ep <= 100U) ? (uint8_t)ep : (uint8_t)ENDPARK_OFF;

    /* Position courante = reference 0 tant qu'aucun homing n'est fait : les
       axes repondent immediatement a SimHub (comme les servos M1-M4 F103). */
    ax[i].calibrated = (m != 0U);
    ax[i].homed      = true;
    ax[i].homing     = false;
    ax[i].hs         = HS_IDLE;
  }

  /* Vitesse de homing persistee (defaut FSM ~333 sps si invalide). */
  if (nvmValid) {
    uint16_t sps = hwEepromReadU16(EEPROM_HOMINGSPS_ADDR);
    if (sps >= 50U && sps <= 10000U) {
      homingSetSpeed((1000000UL + sps / 2U) / sps, 1U);
    }
  }
}

/**
  * @brief  Generation step/dir – appelee depuis le main loop (pas d'ISR).
  *         STEP et DIR sont repartis sur GPIOA et GPIOB : on construit un
  *         masque STEP et un masque DIR par port, decides sur un SEUL
  *         instantane (meme 'now') pour respecter le setup DIR meme lors
  *         d'une inversion de sens.
  */
/* A6-RS : Fmax ~200 kHz, largeur impulsion mini ~2.5 us, setup DIR ~5 us. */
#define DIRECTION_DELAY_US  5U
#define PULSE_HIGH_US       3U
#define PULSE_LOW_US        2U
#define MIN_INTER_STEP_US   6U

static void stepDirServiceFastV2(void)
{
  if (!servoEnabled) return;
  if ((int32_t)(HAL_GetTick() - servoMotionAllowedAt) < 0) return;

  uint16_t * const lastStepUs = s_lastStepUs;
  int8_t   * const dirV       = s_dirV;

  uint16_t now = (uint16_t)TIM2->CNT;

  uint16_t stepMaskA = 0, stepMaskB = 0;
  uint32_t dirSetA = 0, dirClrA = 0, dirSetB = 0, dirClrB = 0;
  bool     stepping   = false;
  bool     dirChanged = false;

  /* Pass 1 : selectionner les moteurs a impulser MAINTENANT et poser DIR. */
  for (uint8_t i = 0; i < NUM_MOTORS; i++)
  {
    if (ax[i].pos == ax[i].target) continue;
    if ((uint16_t)(now - lastStepUs[i]) < MIN_INTER_STEP_US) continue;

    int8_t nd = (ax[i].target > ax[i].pos) ? 1 : -1;
    if (nd < 0 && ax[i].pos == 0U) { ax[i].target = 0U; continue; }

    if (nd != dirV[i])
    {
      dirV[i] = nd;
      /* V2.3 : polarite DIR inversee (drivers/cablage) -> DIR LOW = sens +,
         HIGH = sens -. La position logique (Pass 2) reste inchangee. */
      if (DIR_PORT[i] == GPIOA) {
        if (nd > 0) dirClrA |= DIR_PIN[i]; else dirSetA |= DIR_PIN[i];
      } else {
        if (nd > 0) dirClrB |= DIR_PIN[i]; else dirSetB |= DIR_PIN[i];
      }
      dirChanged = true;
    }

    if (STEP_PORT[i] == GPIOA) stepMaskA |= STEP_PIN[i];
    else                       stepMaskB |= STEP_PIN[i];
    stepping = true;
  }

  if (!stepping) return;

  /* Appliquer les changements de DIR (BSRR : [15:0]=set, [31:16]=reset). */
  if (dirChanged) {
    if (dirSetA || dirClrA) GPIOA->BSRR = dirSetA | (dirClrA << 16);
    if (dirSetB || dirClrB) GPIOB->BSRR = dirSetB | (dirClrB << 16);
    delay_us(DIRECTION_DELAY_US);
  }

  /* Pulse simultane (fronts montants). */
  if (stepMaskA) GPIOA->BSRR = stepMaskA;
  if (stepMaskB) GPIOB->BSRR = stepMaskB;
  delay_us(PULSE_HIGH_US);
  if (stepMaskA) GPIOA->BRR = stepMaskA;
  if (stepMaskB) GPIOB->BRR = stepMaskB;
  delay_us(PULSE_LOW_US);

  /* Pass 2 : mise a jour positions + horodatages. */
  uint16_t newNow = (uint16_t)TIM2->CNT;
  for (uint8_t i = 0; i < NUM_MOTORS; i++)
  {
    const bool pulsed = (STEP_PORT[i] == GPIOA) ? ((stepMaskA & STEP_PIN[i]) != 0U)
                                                : ((stepMaskB & STEP_PIN[i]) != 0U);
    if (!pulsed) continue;
    if (dirV[i] > 0) ax[i].pos++;
    else             ax[i].pos--;
    lastStepUs[i] = newNow;
  }
}

/* ==========================================================================
 *  Protocole texte Motion Center (coexiste avec le binaire SimHub)
 * ========================================================================== */

/* Endpark (SH_DISABLE) : rejoint doucement endParkPct% de la course utile
   (max - 2*margin) sur les axes eligibles AVANT la coupure servo. Bloquant :
   le stepping est entretenu ici meme, cadence = vitesse de homing. */
static void endParkRun(void)
{
  if (!servoEnabled) return;

  uint16_t tgt[NUM_MOTORS] = {0};
  bool     en [NUM_MOTORS] = {false};
  uint16_t maxDelta = 0U;
  bool     anyEn = false;

  for (uint8_t i = 0; i < NUM_MOTORS; i++) {
    if (axCfg[i].endParkPct > 100U) continue;      /* off */
    if (!axCfg[i].connected || !ax[i].homed || axCfg[i].maxPos == 0U) continue;
    const uint16_t mx = axCfg[i].maxPos, mg = axCfg[i].margin;
    const uint16_t course = (mx > (uint16_t)(2U * mg)) ? (uint16_t)(mx - 2U * mg) : 0U;
    tgt[i] = (uint16_t)(((uint32_t)course * axCfg[i].endParkPct) / 100UL);
    en[i]  = true;
    anyEn  = true;
    const uint16_t pos = ax[i].pos;
    const uint16_t d = (pos > tgt[i]) ? (uint16_t)(pos - tgt[i])
                                      : (uint16_t)(tgt[i] - pos);
    if (d > maxDelta) maxDelta = d;
  }
  if (!anyEn) return;

  const uint32_t interval = homingGetIntervalUs();
  const uint8_t  stepSz   = homingGetStepSize();
  const uint16_t sps      = homingGetSps();
  const uint32_t timeoutMs = (uint32_t)maxDelta * 1000UL / (sps ? sps : 1U) + 5000UL;
  const uint32_t t0 = HAL_GetTick();
  uint32_t next = axMicros();

  for (;;) {
    bool pending = false;
    for (uint8_t i = 0; i < NUM_MOTORS; i++) {
      if (en[i] && ax[i].pos != tgt[i]) { pending = true; break; }
    }
    if (!pending) break;
    if ((uint32_t)(HAL_GetTick() - t0) > timeoutMs) break;   /* filet */

    const uint32_t nowus = axMicros();
    if ((int32_t)(nowus - next) >= 0) {
      next += interval;
      for (uint8_t i = 0; i < NUM_MOTORS; i++) {
        if (!en[i]) continue;
        const uint16_t pos = ax[i].pos;
        if (pos == tgt[i]) continue;
        if (pos > tgt[i]) {
          const uint16_t d = (uint16_t)(pos - tgt[i]);
          ax[i].target = (uint16_t)(pos - ((d > stepSz) ? stepSz : d));
        } else {
          uint32_t nt = (uint32_t)pos + stepSz;
          if (nt > tgt[i]) nt = tgt[i];
          ax[i].target = (uint16_t)nt;
        }
      }
    }
    stepDirServiceFastV2();
  }
}

/* Envoi d'une chaine ASCII sur l'USB CDC.
   CRITIQUE : CDC_Transmit_FS ne COPIE PAS le buffer -> buffer statique +
   attente de fin de transfert precedent (paquets de continuation lus dans
   l'IRQ USB). */
static uint8_t s_cdcTxBuf[192];
static void cdc_send_str(const char *s)
{
  uint16_t n = (uint16_t)strlen(s);
  if (n == 0U) return;
  if (n > (uint16_t)sizeof(s_cdcTxBuf)) n = (uint16_t)sizeof(s_cdcTxBuf);

  USBD_CDC_HandleTypeDef *hcdc =
      (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;
  if (hcdc == NULL) return;                  /* USB pas encore enumere */
  for (uint16_t k = 0; (k < 500U) && (hcdc->TxState != 0U); k++)
    delay_us(100U);
  if (hcdc->TxState != 0U) return;           /* lien mort : on jette */

  memcpy(s_cdcTxBuf, s, n);
  for (uint16_t k = 0; k < 64U; k++)
  {
    if (CDC_Transmit_FS(s_cdcTxBuf, n) == USBD_OK) return;
    delay_us(200U);
  }
}

static void txtToUpper(char *s)
{
  for (; *s; ++s)
    if (*s >= 'a' && *s <= 'z') *s = (char)(*s - 32);
}

static bool txtEq(const char *a, const char *b)
{
  while (*a && *b) { if (*a != *b) return false; ++a; ++b; }
  return (*a == '\0' && *b == '\0');
}

/* Ligne STATUS compatible Motion Center (prefixe "STATUS ").
   6 axes : conn,cal,max,margin,hdir,endpark reels. */
static void sendStatusLine(void)
{
  static char line[192];
  int off = snprintf(line, sizeof(line),
                     "STATUS FW=2.3.0 BOX=%u SERVO=%c",
                     (unsigned)boxCfg.version,
                     servoEnabled ? '1' : '0');
  if (off < 0) return;
  for (uint8_t i = 0; i < NUM_MOTORS && off < (int)sizeof(line); i++) {
    int w = snprintf(line + off, sizeof(line) - (size_t)off,
                     " M%u=%u,%u,%u,%u,%u,%u", (unsigned)(i + 1),
                     axCfg[i].connected ? 1U : 0U,
                     ax[i].calibrated   ? 1U : 0U,
                     (unsigned)axCfg[i].maxPos,
                     (unsigned)axCfg[i].margin,
                     axCfg[i].hometoMax ? 1U : 0U,
                     (unsigned)axCfg[i].endParkPct);
    if (w < 0) break;
    off += w;
  }
  if (off < (int)sizeof(line) - 1) { line[off++] = '\n'; line[off] = '\0'; }
  cdc_send_str(line);
}

/* Passage en mode DFU : saut vers le BOOTLOADER SYSTEME en ROM (0x1FFF0000)
   du STM32G431 -> l'appareil reapparait en peripherique USB DFU (VID 0483 /
   PID DF11), flashable par STM32CubeProgrammer / dfu-util. Les drives sont
   d'abord coupes par securite. Ne revient jamais.
   (Alternative materielle equivalente : BOOT0 = 1 au reset.) */
#define SYSMEM_BOOT_ADDR 0x1FFF0000UL
static void enterDFU(void)
{
  disableServo();

  /* Detacher proprement l'USB pour que l'hote re-enumere le peripherique DFU. */
  USBD_Stop(&hUsbDeviceFS);
  USBD_DeInit(&hUsbDeviceFS);
  HAL_Delay(50);

  __disable_irq();
  HAL_RCC_DeInit();
  HAL_DeInit();
  SysTick->CTRL = 0; SysTick->LOAD = 0; SysTick->VAL = 0;

  /* Remappe la memoire systeme a 0x00000000 puis saute a sa table de vecteurs. */
  __HAL_RCC_SYSCFG_CLK_ENABLE();
  __HAL_SYSCFG_REMAPMEMORY_SYSTEMFLASH();
  const uint32_t *sysmem = (const uint32_t *)SYSMEM_BOOT_ADDR;
  __set_MSP(sysmem[0]);
  void (*sysBoot)(void) = (void (*)(void))sysmem[1];
  sysBoot();

  while (1) { }
}

/* ==========================================================================
 *  Commandes de calibration M1..M6 (protocole identique a l'AVR v1.8)
 * ========================================================================== */

static char* nextTok(char **cur)
{
  char *s = *cur;
  while (*s == ' ' || *s == '\t') s++;
  if (*s == '\0') { *cur = s; return NULL; }
  char *tok = s;
  while (*s && *s != ' ' && *s != '\t') s++;
  if (*s) { *s = '\0'; s++; }
  *cur = s;
  return tok;
}

static bool parseU32(const char *s, uint32_t *out)
{
  if (!s || !*s) return false;
  uint32_t v = 0;
  for (; *s; s++) {
    if (*s < '0' || *s > '9') return false;
    if (v > 429496728UL) return false;
    v = v * 10UL + (uint32_t)(*s - '0');
  }
  *out = v;
  return true;
}

static void markSchema(void)
{
  if (hwEepromReadU16(EEPROM_HAS_BEEN_FACTORYRESET) != EEPROM_SCHEMA_VERSION)
    hwEepromWriteU16(EEPROM_HAS_BEEN_FACTORYRESET, EEPROM_SCHEMA_VERSION);
}

/* Persiste max+margin d'un axe (motor = 1..6). */
static void saveCalibration(uint8_t motor)
{
  if (motor < 1U || motor > NUM_MOTORS) return;
  const uint8_t idx = (uint8_t)(motor - 1U);
  hwEepromWriteU16(NVM_MAX[idx],    axCfg[idx].maxPos);
  hwEepromWriteU16(NVM_MARGIN[idx], axCfg[idx].margin);
  markSchema();
}

/* Reponse differee : OK/ERR quand la sequence de calibration se termine. */
static void serviceCalibReply(void)
{
  if (s_calOp == CALOP_NONE) return;

  for (uint8_t i = 0; i < NUM_MOTORS; i++) {
    if (!(s_calMask & (1u << i))) continue;
    if (s_calOp == CALOP_DETECT_MIN) {
      if (ax[i].homing) return;               /* encore en cours */
    } else {
      if (calibIsBusy(i)) return;             /* encore en cours */
    }
  }

  bool ok = true;
  for (uint8_t i = 0; i < NUM_MOTORS; i++) {
    if (!(s_calMask & (1u << i))) continue;
    if (s_calOp == CALOP_DETECT_MIN) {
      ok = ok && ax[i].homed && (ax[i].hs != HS_ERROR);
    } else {
      ok = ok && (calibGetState(i) == CAL_DONE);
      if (calibGetState(i) == CAL_DONE) saveCalibration((uint8_t)(i + 1U));
      calibAckResult(i);
    }
  }
  cdc_send_str(ok ? "OK\n" : "ERR CALIB_FAILED\n");
  s_calOp   = CALOP_NONE;
  s_calMask = 0;
}

/* SET <KEY> <VALUE> [MOTOR] — protocole AVR, applique a M1..M6. */
static void handleSet(char *cursor)
{
  char *key = nextTok(&cursor);
  char *val = nextTok(&cursor);
  char *mot = nextTok(&cursor);
  if (!key || !val) { cdc_send_str("ERR BAD_ARGS\n"); return; }

  uint32_t v32 = 0;
  if (!parseU32(val, &v32)) { cdc_send_str("ERR BAD_VALUE\n"); return; }
  if (v32 > 65535UL) v32 = 65535UL;

  if (txtEq(key, "HOMING_S")) {
    uint32_t sps = v32;
    if (sps < 50UL)    sps = 50UL;
    if (sps > 10000UL) sps = 10000UL;
    cdc_send_str("OK\n");                  /* repondre AVANT l'ecriture flash */
    homingSetSpeed((1000000UL + sps / 2UL) / sps, 1U);
    hwEepromWriteU16(EEPROM_HOMINGSPS_ADDR, (uint16_t)sps);
    return;
  }

  uint32_t motor = 0;
  if (!mot || !parseU32(mot, &motor) || motor < 1U || motor > NUM_MOTORS) {
    cdc_send_str("ERR BAD_MOTOR\n");
    return;
  }
  const uint8_t idx = (uint8_t)(motor - 1U);

  if (txtEq(key, "CONNECTED")) {
    cdc_send_str("OK\n");
    axCfg[idx].connected = (v32 != 0UL);
    if (v32 == 0UL) { ax[idx].homed = false; ax[idx].target = ax[idx].pos; }
    hwEepromWriteU16(NVM_CONN[idx], (v32 != 0UL) ? 1U : 0U);
    markSchema();
    return;
  }

  if (txtEq(key, "MAX")) {
    cdc_send_str("OK\n");
    axCfg[idx].maxPos = (uint16_t)v32;
    ax[idx].calibrated = (v32 != 0UL);
    saveCalibration((uint8_t)motor);
    return;
  }
  if (txtEq(key, "MARGIN")) {
    cdc_send_str("OK\n");
    axCfg[idx].margin = (uint16_t)v32;
    saveCalibration((uint8_t)motor);
    return;
  }
  if (txtEq(key, "HOMING_DIR")) {
    bool toMax = (v32 != 0UL);
    if (toMax && !HomingDir_MaxAllowed(axCfg[idx].maxPos)) {
      cdc_send_str("ERR MAX_REQUIRED\n");
      return;
    }
    cdc_send_str("OK\n");
    axCfg[idx].hometoMax = toMax;
    ax[idx].homed = false;         /* re-homing requis avec la nouvelle direction */
    hwEepromWriteU16(NVM_HDIR[idx], toMax ? 1U : 0U);
    markSchema();
    return;
  }
  if (txtEq(key, "ENDPARK")) {
    cdc_send_str("OK\n");
    axCfg[idx].endParkPct = (v32 <= 100UL) ? (uint8_t)v32 : (uint8_t)ENDPARK_OFF;
    hwEepromWriteU16(NVM_EPARK[idx], (uint16_t)axCfg[idx].endParkPct);
    markSchema();
    return;
  }
  cdc_send_str("ERR UNKNOWN_KEY\n");
}

/* Une commande de calibration sur un axe non connecte le connecte (regle AVR). */
static void autoConnectIfNeeded(uint8_t motor)
{
  if (motor < 1U || motor > NUM_MOTORS) return;
  const uint8_t idx = (uint8_t)(motor - 1U);
  if (axCfg[idx].connected) return;
  axCfg[idx].connected = true;
  hwEepromWriteU16(NVM_CONN[idx], 1U);
  markSchema();
}

/* DO <ACTION> [MOTOR] — actions de calibration M1..M6 (protocole AVR). */
static void handleDo(char *cursor)
{
  char *act = nextTok(&cursor);
  char *mot = nextTok(&cursor);
  if (!act) { cdc_send_str("ERR BAD_ARGS\n"); return; }

  uint32_t motor = 0;
  if (mot && !parseU32(mot, &motor)) { cdc_send_str("ERR BAD_MOTOR\n"); return; }

  if (txtEq(act, "CANCEL")) {
    homingAbortRequest();
    if (s_calOp != CALOP_NONE) { s_calOp = CALOP_NONE; s_calMask = 0; }
    cdc_send_str("OK\n");
    return;
  }

  if (txtEq(act, "DETECT_MIN")) {
    if (motor > NUM_MOTORS) { cdc_send_str("ERR BAD_MOTOR\n"); return; }
    if (s_calOp != CALOP_NONE) { cdc_send_str("ERR BUSY\n"); return; }
    if (motor != 0U) autoConnectIfNeeded((uint8_t)motor);
    uint8_t mask = 0;
    bool anyTargeted = false;
    for (uint8_t i = 0; i < NUM_MOTORS; i++) {
      if ((motor == 0U || (motor - 1U) == i) && axCfg[i].connected) {
        anyTargeted = true;
        /* V2.3 : M1-M4 (servos A6-RS) reputes cales -> pas de seek. */
        if (i < 4U) { ax[i].homed = true; }
        else        { ax[i].homed = false; mask |= (uint8_t)(1u << i); }
      }
    }
    if (!anyTargeted) { cdc_send_str("ERR NOT_CONNECTED\n"); return; }
    if (!mask) { cdc_send_str("OK\n"); return; }   /* que des M1-M4 : calibre direct */
    if (!servoEnabled) enableServo();
    homingStart(mask);
    s_calOp = CALOP_DETECT_MIN; s_calMask = mask;
    return;
  }

  if (txtEq(act, "DETECT_MAX") || txtEq(act, "MOVE_TO_MAX")) {
    if (motor < 1U || motor > NUM_MOTORS) { cdc_send_str("ERR BAD_MOTOR\n"); return; }
    if (s_calOp != CALOP_NONE) { cdc_send_str("ERR BUSY\n"); return; }
    const uint8_t idx = (uint8_t)(motor - 1U);
    autoConnectIfNeeded((uint8_t)motor);
    /* V2.3 : M1-M4 reputes cales -> renvoyer calibre sans mesurer le MAX. */
    if (idx < 4U) {
      ax[idx].homed = true; ax[idx].calibrated = true;
      if (axCfg[idx].maxPos == 0U) axCfg[idx].maxPos = 32767U;
      cdc_send_str("OK\n"); return;
    }
    if (!ax[idx].homed) { cdc_send_str("ERR NOT_HOMED\n"); return; }
    if (!servoEnabled) enableServo();
    calibStartDetectMax(idx);
    s_calOp = CALOP_CALIB; s_calMask = (uint8_t)(1u << idx);
    return;
  }

  if (txtEq(act, "FULL_CALIB") || txtEq(act, "COMPLETE_CALIB")) {
    if (motor < 1U || motor > NUM_MOTORS) { cdc_send_str("ERR BAD_MOTOR\n"); return; }
    if (s_calOp != CALOP_NONE) { cdc_send_str("ERR BUSY\n"); return; }
    const uint8_t idx = (uint8_t)(motor - 1U);
    autoConnectIfNeeded((uint8_t)motor);
    /* V2.3 : M1-M4 reputes cales -> renvoyer calibre sans seek MIN/MAX. */
    if (idx < 4U) {
      ax[idx].homed = true; ax[idx].calibrated = true;
      if (axCfg[idx].maxPos == 0U) axCfg[idx].maxPos = 32767U;
      cdc_send_str("OK\n"); return;
    }
    if (!servoEnabled) enableServo();
    calibStartFull(idx);
    s_calOp = CALOP_CALIB; s_calMask = (uint8_t)(1u << idx);
    return;
  }

  if (txtEq(act, "SERVO_ON"))  { enableServo();  cdc_send_str("OK\n"); return; }
  if (txtEq(act, "SERVO_OFF")) {
    homingCancel(); calibCancel();
    s_calOp = CALOP_NONE; s_calMask = 0;
    disableServo();
    cdc_send_str("OK\n");
    return;
  }

  cdc_send_str("ERR UNKNOWN_ACTION\n");
}

/* Dispatch d'une ligne de commande texte (terminee, sans \n). */
static void handleTextCommand(const char *line)
{
  char buf[48];

  uint8_t i = 0;
  while (line[i] == ' ' || line[i] == '\t') i++;
  uint8_t j = 0;
  while (line[i] && j < (uint8_t)(sizeof(buf) - 1U)) buf[j++] = line[i++];
  while (j > 0U && (buf[j - 1] == ' ' || buf[j - 1] == '\t')) j--;
  buf[j] = '\0';
  if (j == 0U) return;

  txtToUpper(buf);

  /* --- Bootloader DFU : !DFU --------------------------------------------- */
  if (txtEq(buf, "!DFU")) {
    cdc_send_str("DFU\n");
    HAL_Delay(50);
    enterDFU();      /* ne revient pas */
    return;
  }

  /* --- Handshake Motion Center ------------------------------------------ */
  if (txtEq(buf, "MC_START") || txtEq(buf, "HELLO")) {
    simhubConnected = true;
    hostConnected   = true;
    cdc_send_str("OK_MC_CONNECTED\n");
    return;
  }
  if (txtEq(buf, "MC_DISABLE")) {
    disableServo();
    cdc_send_str("OK_MC_DISABLED\n");
    return;
  }
  if (txtEq(buf, "MC_STOP") || txtEq(buf, "DISCONNECT")) {
    cdc_send_str("OK_MC_DISCONNECTED\n");
    return;
  }
  if (txtEq(buf, "GET")) { sendStatusLine(); return; }
  if (txtEq(buf, "ENABLE"))  { enableServo();  cdc_send_str("OK\n"); return; }
  if (txtEq(buf, "DISABLE")) { disableServo(); cdc_send_str("OK\n"); return; }

  /* SET / DO : sous-commandes de calibration. */
  if (buf[0] == 'S' && buf[1] == 'E' && buf[2] == 'T' && (buf[3] == ' ' || buf[3] == '\0')) {
    char *cur = buf + 3; handleSet(cur); return;
  }
  if (buf[0] == 'D' && buf[1] == 'O' && (buf[2] == ' ' || buf[2] == '\0')) {
    char *cur = buf + 2; handleDo(cur); return;
  }

  /* --- Handshake SimHub (variante texte) -------------------------------- */
  if (txtEq(buf, "SH_CONNECTED") || txtEq(buf, "SHCONNECTED") || txtEq(buf, "SH-CONNECTED")) {
    simhubConnected = true;
    hostConnected   = true;
    cdc_send_str("SHCONNECTED\n");
    return;
  }
  if (txtEq(buf, "SH_START")) {
    for (uint8_t k = 0; k < NUM_MOTORS; k++) {
      ax[k].pos = 0U; ax[k].target = 0U; s_lastStepUs[k] = 0U; s_dirV[k] = 0;
      ax[k].homed = true;    /* position courante = reference 0 */
    }
    simhubConnected = true;
    hostConnected   = true;
    enableServo();
    /* Les drivers viennent d'etre alimentes par le relais : laisser le temps
       de s'initialiser avant de repondre CALIBRATED (SimHub streame ensuite). */
    HAL_Delay(DRIVER_INIT_MS);
    cdc_send_str("CALIBRATED\n");
    return;
  }
  if (txtEq(buf, "SH_DISABLE")) {
    endParkRun();      /* Endpark : rejoindre doucement le % configure */
    simhubConnected = false;
    hostConnected   = false;
    homingCancel(); calibCancel();
    disableServo();
    cdc_send_str("SH_DISABLED\n");
    return;
  }
  if (txtEq(buf, "SH_DOWN") || txtEq(buf, "SHDOWN") || txtEq(buf, "SH-DOWN")) {
    simhubConnected = false;
    hostConnected   = false;
    disableServo();
    return;
  }

  /* Inconnu : ignore silencieusement (robustesse SimHub). */
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}

#ifdef  USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  (void)file; (void)line;
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
