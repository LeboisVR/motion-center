/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   SRT80 PRO Control Box V2.3 — STM32G431CBT6.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32g4xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
/* =====================================================================
 *  Pinout PRO V2.3 (PCB V2.3) — STM32G431CBT6
 *
 *  STEP (S1..S6)  : impulsion de pas (sortie push-pull)
 *  DIR  (D1..D6)  : sens              (sortie push-pull)
 *  M1..M6 ENDSTOP : butee par moteur  (entree, switch actif bas / pull-up)
 *  IN1/IN2        : entrees reservees (non utilisees pour l'instant)
 *  ENABLE_RELAY   : relais d'alimentation drivers (sortie, actif haut)
 *
 *  Repartition des ports STEP/DIR (important pour le code de stepping) :
 *    M1,M2,M5,M6 -> GPIOB   |   M3,M4 -> GPIOA
 * ===================================================================== */

/* ---- STEP ---- */
#define S1_Pin        GPIO_PIN_4
#define S1_GPIO_Port  GPIOB
#define S2_Pin        GPIO_PIN_9
#define S2_GPIO_Port  GPIOB
#define S3_Pin        GPIO_PIN_4
#define S3_GPIO_Port  GPIOA
#define S4_Pin        GPIO_PIN_6
#define S4_GPIO_Port  GPIOA
#define S5_Pin        GPIO_PIN_14
#define S5_GPIO_Port  GPIOB
#define S6_Pin        GPIO_PIN_12
#define S6_GPIO_Port  GPIOB

/* ---- DIR ---- */
#define D1_Pin        GPIO_PIN_5
#define D1_GPIO_Port  GPIOB
#define D2_Pin        GPIO_PIN_7
#define D2_GPIO_Port  GPIOB
#define D3_Pin        GPIO_PIN_5
#define D3_GPIO_Port  GPIOA
#define D4_Pin        GPIO_PIN_7
#define D4_GPIO_Port  GPIOA
#define D5_Pin        GPIO_PIN_13
#define D5_GPIO_Port  GPIOB
#define D6_Pin        GPIO_PIN_15
#define D6_GPIO_Port  GPIOB

/* ---- Endstops (un par moteur) ---- */
#define M1ENDSTOP_Pin        GPIO_PIN_3
#define M1ENDSTOP_GPIO_Port  GPIOB
#define M2ENDSTOP_Pin        GPIO_PIN_6
#define M2ENDSTOP_GPIO_Port  GPIOB
#define M3ENDSTOP_Pin        GPIO_PIN_13
#define M3ENDSTOP_GPIO_Port  GPIOC
#define M4ENDSTOP_Pin        GPIO_PIN_0
#define M4ENDSTOP_GPIO_Port  GPIOB
#define M5ENDSTOP_Pin        GPIO_PIN_1
#define M5ENDSTOP_GPIO_Port  GPIOB
#define M6ENDSTOP_Pin        GPIO_PIN_9
#define M6ENDSTOP_GPIO_Port  GPIOA

/* ---- Entrees reservees ---- */
#define IN1_Pin        GPIO_PIN_2
#define IN1_GPIO_Port  GPIOA
#define IN2_Pin        GPIO_PIN_3
#define IN2_GPIO_Port  GPIOA

/* ---- Relais d'alimentation drivers (actif haut) ---- */
#define ENABLE_RELAY_Pin        GPIO_PIN_11
#define ENABLE_RELAY_GPIO_Port  GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
