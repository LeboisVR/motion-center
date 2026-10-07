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

void HAL_TIM_MspPostInit(TIM_HandleTypeDef *htim);

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define S1_Pin GPIO_PIN_2
#define S1_GPIO_Port GPIOE
#define READYM4_Pin GPIO_PIN_3
#define READYM4_GPIO_Port GPIOE
#define READYM3_Pin GPIO_PIN_4
#define READYM3_GPIO_Port GPIOE
#define READYM2_Pin GPIO_PIN_5
#define READYM2_GPIO_Port GPIOE
#define READYM1_Pin GPIO_PIN_6
#define READYM1_GPIO_Port GPIOE
#define READYM5_Pin GPIO_PIN_9
#define READYM5_GPIO_Port GPIOF
#define READYM6_Pin GPIO_PIN_10
#define READYM6_GPIO_Port GPIOF
#define READYM7_Pin GPIO_PIN_0
#define READYM7_GPIO_Port GPIOC
#define SI5_Pin GPIO_PIN_1
#define SI5_GPIO_Port GPIOC
#define SI6_Pin GPIO_PIN_2
#define SI6_GPIO_Port GPIOC
#define SI7_Pin GPIO_PIN_3
#define SI7_GPIO_Port GPIOC
#define WS2812_Pin GPIO_PIN_4
#define WS2812_GPIO_Port GPIOA
#define S4_Pin GPIO_PIN_6
#define S4_GPIO_Port GPIOA
#define S5_Pin GPIO_PIN_7
#define S5_GPIO_Port GPIOA
#define ENABLEM5_Pin GPIO_PIN_4
#define ENABLEM5_GPIO_Port GPIOC
#define ENABLEM6_Pin GPIO_PIN_5
#define ENABLEM6_GPIO_Port GPIOC
#define ENABLEM7_Pin GPIO_PIN_0
#define ENABLEM7_GPIO_Port GPIOB
#define RELAIS_Pin GPIO_PIN_7
#define RELAIS_GPIO_Port GPIOE
#define ENDSTOPM1_Pin GPIO_PIN_8
#define ENDSTOPM1_GPIO_Port GPIOE
#define ENDSTOPM2_Pin GPIO_PIN_9
#define ENDSTOPM2_GPIO_Port GPIOE
#define ENDSTOPM3_Pin GPIO_PIN_10
#define ENDSTOPM3_GPIO_Port GPIOE
#define ENDSTOPM4_Pin GPIO_PIN_11
#define ENDSTOPM4_GPIO_Port GPIOE
#define ENDSTOPM5_Pin GPIO_PIN_12
#define ENDSTOPM5_GPIO_Port GPIOE
#define ENDSTOPM6_Pin GPIO_PIN_13
#define ENDSTOPM6_GPIO_Port GPIOE
#define ENDSTOPM7_Pin GPIO_PIN_14
#define ENDSTOPM7_GPIO_Port GPIOE
#define SO7_Pin GPIO_PIN_10
#define SO7_GPIO_Port GPIOD
#define S2_Pin GPIO_PIN_12
#define S2_GPIO_Port GPIOD
#define SO4_Pin GPIO_PIN_13
#define SO4_GPIO_Port GPIOD
#define ESTOP_Pin GPIO_PIN_15
#define ESTOP_GPIO_Port GPIOD
#define S3_Pin GPIO_PIN_6
#define S3_GPIO_Port GPIOC
#define SO6_Pin GPIO_PIN_7
#define SO6_GPIO_Port GPIOC
#define SO5_Pin GPIO_PIN_8
#define SO5_GPIO_Port GPIOC
#define SI2_Pin GPIO_PIN_9
#define SI2_GPIO_Port GPIOC
#define SI1_Pin GPIO_PIN_8
#define SI1_GPIO_Port GPIOA
#define SI3_Pin GPIO_PIN_9
#define SI3_GPIO_Port GPIOA
#define SI4_Pin GPIO_PIN_10
#define SI4_GPIO_Port GPIOA
#define ENABLEM4_Pin GPIO_PIN_10
#define ENABLEM4_GPIO_Port GPIOC
#define ENABLEM3_Pin GPIO_PIN_11
#define ENABLEM3_GPIO_Port GPIOC
#define ENABLEM1_Pin GPIO_PIN_12
#define ENABLEM1_GPIO_Port GPIOC
#define ENABLEM2_Pin GPIO_PIN_0
#define ENABLEM2_GPIO_Port GPIOD
#define D1_Pin GPIO_PIN_1
#define D1_GPIO_Port GPIOD
#define D2_Pin GPIO_PIN_2
#define D2_GPIO_Port GPIOD
#define D3_Pin GPIO_PIN_3
#define D3_GPIO_Port GPIOD
#define D4_Pin GPIO_PIN_4
#define D4_GPIO_Port GPIOD
#define D5_Pin GPIO_PIN_5
#define D5_GPIO_Port GPIOD
#define D6_Pin GPIO_PIN_6
#define D6_GPIO_Port GPIOD
#define D7_Pin GPIO_PIN_7
#define D7_GPIO_Port GPIOD
#define SO3_Pin GPIO_PIN_3
#define SO3_GPIO_Port GPIOB
#define SO2_Pin GPIO_PIN_4
#define SO2_GPIO_Port GPIOB
#define S6_Pin GPIO_PIN_5
#define S6_GPIO_Port GPIOB
#define S7_Pin GPIO_PIN_6
#define S7_GPIO_Port GPIOB
#define SO1_Pin GPIO_PIN_7
#define SO1_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
