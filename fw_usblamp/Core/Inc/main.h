/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
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
#include "stm32u0xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* Export the USB PCD handle so USBX app code can use it. */
extern PCD_HandleTypeDef hpcd_USB_DRD_FS;

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

void HAL_LPTIM_MspPostInit(LPTIM_HandleTypeDef *hlptim);

void HAL_TIM_MspPostInit(TIM_HandleTypeDef *htim);

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define B1_Pin GPIO_PIN_0
#define B1_GPIO_Port GPIOA
#define B2_Pin GPIO_PIN_2
#define B2_GPIO_Port GPIOA
#define USB_Pin GPIO_PIN_3
#define USB_GPIO_Port GPIOA
#define BUZZ_Pin GPIO_PIN_4
#define BUZZ_GPIO_Port GPIOA
#define AN_LIGHT_Pin GPIO_PIN_7
#define AN_LIGHT_GPIO_Port GPIOA
#define AN_BATT_Pin GPIO_PIN_0
#define AN_BATT_GPIO_Port GPIOB
#define LED_Pin GPIO_PIN_8
#define LED_GPIO_Port GPIOA
#define CTL_LED_Pin GPIO_PIN_15
#define CTL_LED_GPIO_Port GPIOA
#define STA_CHG_Pin GPIO_PIN_3
#define STA_CHG_GPIO_Port GPIOB
#define CTL_CEN_Pin GPIO_PIN_4
#define CTL_CEN_GPIO_Port GPIOB
#define CTL_LEN_Pin GPIO_PIN_5
#define CTL_LEN_GPIO_Port GPIOB
#define BL_Pin GPIO_PIN_3
#define BL_GPIO_Port GPIOF

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
