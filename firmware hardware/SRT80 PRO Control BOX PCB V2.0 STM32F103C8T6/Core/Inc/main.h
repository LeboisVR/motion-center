/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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
#include "stm32f1xx_hal.h"

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
#define M5ENDSTOP_Pin GPIO_PIN_13
#define M5ENDSTOP_GPIO_Port GPIOC
#define S6_Pin GPIO_PIN_1
#define S6_GPIO_Port GPIOA
#define S5_Pin GPIO_PIN_2
#define S5_GPIO_Port GPIOA
#define S4_Pin GPIO_PIN_3
#define S4_GPIO_Port GPIOA
#define S3_Pin GPIO_PIN_4
#define S3_GPIO_Port GPIOA
#define S2_Pin GPIO_PIN_5
#define S2_GPIO_Port GPIOA
#define S1_Pin GPIO_PIN_6
#define S1_GPIO_Port GPIOA
#define M6ENDSTOP_Pin GPIO_PIN_7
#define M6ENDSTOP_GPIO_Port GPIOA
#define D1_Pin GPIO_PIN_0
#define D1_GPIO_Port GPIOB
#define D2_Pin GPIO_PIN_1
#define D2_GPIO_Port GPIOB
#define LED_Pin GPIO_PIN_11
#define LED_GPIO_Port GPIOB
/* Variante PCB :
   - PCB_V21=0 : PCB V2.0 (PRO Control Box V1) — relais PB12 (actif haut),
                 enable servo PB14 (actif bas), homing MIN M5/M6 au SH_START.
   - PCB_V21=1 : PCB V2.1 (PRO Control Box V2) — relais seul sur PB14 (actif
                 haut), PB12 non câblé, pas de homing MIN M5/M6 au start. */
#ifndef PCB_V21
#define PCB_V21 0
#endif
#if PCB_V21
#define ENABLE_RELAY_Pin GPIO_PIN_14
#define ENABLE_RELAY_GPIO_Port GPIOB
#define ENABLE_SERVO_Pin GPIO_PIN_12   /* non câblé sur PCB V2.1 (inoffensif) */
#define ENABLE_SERVO_GPIO_Port GPIOB
#else
#define ENABLE_RELAY_Pin GPIO_PIN_12
#define ENABLE_RELAY_GPIO_Port GPIOB
#define ENABLE_SERVO_Pin GPIO_PIN_14
#define ENABLE_SERVO_GPIO_Port GPIOB
#endif
#define D6_Pin GPIO_PIN_6
#define D6_GPIO_Port GPIOB
#define D3_Pin GPIO_PIN_7
#define D3_GPIO_Port GPIOB
#define D4_Pin GPIO_PIN_8
#define D4_GPIO_Port GPIOB
#define D5_Pin GPIO_PIN_9
#define D5_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
