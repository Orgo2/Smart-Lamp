/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "app_usbx_device.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "usb_cli.h"
#include "led.h"
#include "analog.h"
#include "alarm.h"
#include "charger.h"
#include "mic.h"
#include "MiniPascal.h"
#include "mp_buttons.h"
#include "lp_delay.h"
#include "rtc.h"
#include "memmon.h"
#include "stm32u0xx_hal_pwr_ex.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
/* Flash memory reserved for user program/data storage. */
extern const uint32_t __flash_data_start__;
extern const uint32_t __flash_data_end__;

#define FLASH_DATA_START ((uint32_t)&__flash_data_start__)
#define FLASH_DATA_END   ((uint32_t)&__flash_data_end__)
#define FLASH_DATA_SIZE  (FLASH_DATA_END - FLASH_DATA_START)

/* RAM info (from linker script). */
extern uint8_t _estack;
#define RAM_START_ADDR 0x20000000u

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
ADC_HandleTypeDef hadc1;

I2C_HandleTypeDef hi2c1;

LPTIM_HandleTypeDef hlptim2;

RNG_HandleTypeDef hrng;

RTC_HandleTypeDef hrtc;

SPI_HandleTypeDef hspi1;
DMA_HandleTypeDef hdma_spi1_rx;

TIM_HandleTypeDef htim2;
DMA_HandleTypeDef hdma_tim2_ch1;

PCD_HandleTypeDef hpcd_USB_DRD_FS;

/* USER CODE BEGIN PV */
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_USB_PCD_Init(void);
static void MX_LPTIM2_Init(void);
static void MX_TIM2_Init(void);
static void MX_I2C1_Init(void);
static void MX_RTC_Init(void);
static void MX_ADC1_Init(void);
static void MX_RNG_Init(void);
static void MX_SPI1_Init(void);
/* USER CODE BEGIN PFP */
static void JumpToBootloader(void);
static void CheckBootloaderEntry(void);
static void BL_Task(void);
void HAL_SYSTICK_Callback(void);
static void LowBattery_Task(void);
static void LowBattery_EarlyGate(void);
static void NoProgram_SleepUntilUSB(void);
static void EnterStop(void);
static void Power_MinimizeLoads(void);
static void EnterShutdown(void);
static void B2_Hold_Service_NoSleep(uint32_t now_ms);
static void B2_Hold_Service_Blocking(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* One-shot charger reset on USB attach (helps STNS01 recover from attach glitches). */
static volatile uint8_t s_usb_chgrst_done = 0u;

/* STOP2 wake bookkeeping (so a short BT1 press reliably starts the lamp after wake). */
static volatile uint8_t s_stop2_armed = 0u;
static volatile uint8_t s_stop2_woke_by_b1 = 0u;

/* Battery policy: after USB detach, do not autorun; wait in STOP2 for B1 hold. */
static volatile uint8_t s_battery_run_allowed = 0u;

/* First-stage GPIO init (bootloader entry check) should not enable EXTI NVIC. */
static volatile uint8_t s_gpio_skip_exti_nvic = 0u;

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
  /* GPIO init for bootloader check (BL held 5s). */
  s_gpio_skip_exti_nvic = 1u;
  MX_GPIO_Init();
  CheckBootloaderEntry();
  s_gpio_skip_exti_nvic = 0u;
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_USB_PCD_Init();
  MX_USBX_Device_Init();
  MX_LPTIM2_Init();
  MX_TIM2_Init();
  MX_I2C1_Init();
  MX_RTC_Init();
  MX_ADC1_Init();
  MX_RNG_Init();
  MX_SPI1_Init();
  /* USER CODE BEGIN 2 */
  /* Enable LED power supply. */
  HAL_GPIO_WritePin(CTL_LEN_GPIO_Port, CTL_LEN_Pin, GPIO_PIN_SET);
  LP_DELAY(100);

  /* Initialize ANALOG driver (ADC with VREFINT calibration). */
  ANALOG_Init(&hadc1);

  /* Initialize charger driver. */
  CHARGER_Init();
  if (USB_IsPresent() != 0u)
  {
    CHARGER_Reset();
    s_usb_chgrst_done = 1u;
  }

  /* If battery is critically low (and no USB), park MCU in standby and retry every 1 s. */
  LowBattery_EarlyGate();

  /* Initialize PDM microphone driver (SPI1+DMA). MIC_Task() updates 50 ms windows. */
  MIC_Init();
  /* MIC_Start() is handled by the driver automatically if needed (interval/continuous). */

  /* Initialize USB CLI (CDC). */
  USB_CLI_Init();
  
  /* Initialize MiniPascal interpreter. */
  mp_init();

  /* Initialize button debouncer/event queue (battery use). */
  MP_Buttons_Init();

  /* Track RAM free/min-free for debugging (CLI MEM). */
  MemMon_Init();

  /* If running on battery and no programs exist, blink and wait in low power until USB is connected. */
  if ((USB_IsPresent() == 0u) && (mp_first_program_slot() == 0u))
  {
    NoProgram_SleepUntilUSB();
  }
  else if (USB_IsPresent() == 0u)
  {
    /* Battery boot OK: 1x blink 200ms */
    IND_LED_On();
    LP_DELAY(200);
    IND_LED_Off();
  }

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
      uint8_t usb_pin = USB_IsPresent();
      static uint8_t s_usb_pin_prev = 0u;

      /* On attach, reset charger once (EXTI attach normally triggers reset; this is a fallback). */
      if ((s_usb_pin_prev == 0u) && (usb_pin != 0u))
      {
        if (!s_usb_chgrst_done)
        {
          CHARGER_Reset();
          s_usb_chgrst_done = 1u;
        }

        /* USB connected: stop any battery program and blank LEDs. */
        mp_force_stop();
        Power_MinimizeLoads();
      }

      /* Fallback: if EXTI detach interrupt is missed, still switch to battery mode. */
      if ((s_usb_pin_prev != 0u) && (usb_pin == 0u))
      {
        USB_CLI_NotifyDetach();
        mp_request_usb_detach();
        IND_LED_Off();
        s_usb_chgrst_done = 0u;
        s_battery_run_allowed = 0u;
      }
      s_usb_pin_prev = usb_pin;

      if (usb_pin)
      {
        ux_device_stack_tasks_run();
        USB_CLI_Task();
      }

      /* Debounced buttons: feed short/long events into MiniPascal (USB + battery). */
      MP_Buttons_Poll(HAL_GetTick());
      while (1)
      {
        mp_btn_id_t e = MP_Buttons_PopShort();
        if (e == MP_BTN_NONE) break;
        mp_notify_button_short((uint8_t)e);
      }
      while (1)
      {
        mp_btn_id_t e = MP_Buttons_PopLong();
        if (e == MP_BTN_NONE) break;
        mp_notify_button_long((uint8_t)e);
      }

      /* USB mode is controlled by USB detect pin. */
      uint8_t usb_mode = usb_pin;
      if (!usb_mode)
      {
        if (mp_first_program_slot() == 0u)
        {
          NoProgram_SleepUntilUSB();
        }
        if (!s_battery_run_allowed)
        {
          EnterStop(); /* waits for B1 hold >=2s */
          continue;
        }
        mp_autorun_poll();
        mp_task();
      }
      ANALOG_Task();
      CHARGER_Task();
      LowBattery_Task();
      BEEP_Task();
      MIC_Task();
      BL_Task();
      /* Battery safety: B2 hold >=2s forces shutdown even if program is running. */
      B2_Hold_Service_NoSleep(HAL_GetTick());
      MemMon_Task();
      /* Power save: sleep until an interrupt (USB/UART/EXTI/SysTick...). */
      __WFI();
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
  RCC_CRSInitTypeDef RCC_CRSInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Configure LSE Drive Capability
  */
  HAL_PWR_EnableBkUpAccess();
  __HAL_RCC_LSEDRIVE_CONFIG(RCC_LSEDRIVE_LOW);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI|RCC_OSCILLATORTYPE_LSE
                              |RCC_OSCILLATORTYPE_HSI48|RCC_OSCILLATORTYPE_MSI;
  RCC_OscInitStruct.LSEState = RCC_LSE_ON;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.MSIState = RCC_MSI_ON;
  RCC_OscInitStruct.MSICalibrationValue = RCC_MSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.MSIClockRange = RCC_MSIRANGE_11;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_MSI;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Enable the CRS clock
  */
  __HAL_RCC_CRS_CLK_ENABLE();

  /** Configures CRS
  */
  RCC_CRSInitStruct.Prescaler = RCC_CRS_SYNC_DIV1;
  RCC_CRSInitStruct.Source = RCC_CRS_SYNC_SOURCE_USB;
  RCC_CRSInitStruct.Polarity = RCC_CRS_SYNC_POLARITY_RISING;
  RCC_CRSInitStruct.ReloadValue = __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000,1000);
  RCC_CRSInitStruct.ErrorLimitValue = 34;
  RCC_CRSInitStruct.HSI48CalibrationValue = 32;

  HAL_RCCEx_CRSConfig(&RCC_CRSInitStruct);
}

/**
  * @brief ADC1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_ADC1_Init(void)
{

  /* USER CODE BEGIN ADC1_Init 0 */

  /* USER CODE END ADC1_Init 0 */

  ADC_ChannelConfTypeDef sConfig = {0};
  ADC_AnalogWDGConfTypeDef AnalogWDGConfig = {0};

  /* USER CODE BEGIN ADC1_Init 1 */

  /* USER CODE END ADC1_Init 1 */

  /** Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
  */
  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV1;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.ScanConvMode = ADC_SCAN_SEQ_FIXED;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  hadc1.Init.LowPowerAutoWait = DISABLE;
  hadc1.Init.LowPowerAutoPowerOff = DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
  hadc1.Init.DMAContinuousRequests = DISABLE;
  hadc1.Init.Overrun = ADC_OVR_DATA_PRESERVED;
  hadc1.Init.SamplingTimeCommon1 = ADC_SAMPLETIME_160CYCLES_5;
  hadc1.Init.OversamplingMode = DISABLE;
  hadc1.Init.TriggerFrequencyMode = ADC_TRIGGER_FREQ_HIGH;
  if (HAL_ADC_Init(&hadc1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_VREFINT;
  sConfig.Rank = ADC_RANK_CHANNEL_NUMBER;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_14;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_17;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Regular Channel
  */
  sConfig.Channel = ADC_CHANNEL_VBAT;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure the WatchDogs 2
  */
  AnalogWDGConfig.WatchdogNumber = ADC_ANALOGWATCHDOG_2;
  AnalogWDGConfig.WatchdogMode = ADC_ANALOGWATCHDOG_SINGLE_REG;
  if (HAL_ADC_AnalogWDGConfig(&hadc1, &AnalogWDGConfig) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure the WatchDogs 3
  */
  AnalogWDGConfig.WatchdogNumber = ADC_ANALOGWATCHDOG_3;
  if (HAL_ADC_AnalogWDGConfig(&hadc1, &AnalogWDGConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN ADC1_Init 2 */

  /* USER CODE END ADC1_Init 2 */

}

/**
  * @brief I2C1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_I2C1_Init(void)
{

  /* USER CODE BEGIN I2C1_Init 0 */

  /* USER CODE END I2C1_Init 0 */

  /* USER CODE BEGIN I2C1_Init 1 */

  /* USER CODE END I2C1_Init 1 */
  hi2c1.Instance = I2C1;
  hi2c1.Init.Timing = 0x20303EFD;
  hi2c1.Init.OwnAddress1 = 0;
  hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
  hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
  hi2c1.Init.OwnAddress2 = 0;
  hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
  hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
  hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
  if (HAL_I2C_Init(&hi2c1) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Analogue filter
  */
  if (HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure Digital filter
  */
  if (HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN I2C1_Init 2 */

  /* USER CODE END I2C1_Init 2 */

}

/**
  * @brief LPTIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_LPTIM2_Init(void)
{

  /* USER CODE BEGIN LPTIM2_Init 0 */

  /* USER CODE END LPTIM2_Init 0 */

  LPTIM_OC_ConfigTypeDef sConfig1 = {0};

  /* USER CODE BEGIN LPTIM2_Init 1 */

  /* USER CODE END LPTIM2_Init 1 */
  hlptim2.Instance = LPTIM2;
  hlptim2.Init.Clock.Source = LPTIM_CLOCKSOURCE_APBCLOCK_LPOSC;
  hlptim2.Init.Clock.Prescaler = LPTIM_PRESCALER_DIV16;
  hlptim2.Init.Trigger.Source = LPTIM_TRIGSOURCE_SOFTWARE;
  hlptim2.Init.Period = 4000;
  hlptim2.Init.UpdateMode = LPTIM_UPDATE_ENDOFPERIOD;
  hlptim2.Init.CounterSource = LPTIM_COUNTERSOURCE_INTERNAL;
  hlptim2.Init.Input1Source = LPTIM_INPUT1SOURCE_GPIO;
  hlptim2.Init.RepetitionCounter = 0;
  if (HAL_LPTIM_Init(&hlptim2) != HAL_OK)
  {
    Error_Handler();
  }
  sConfig1.Pulse = 0;
  sConfig1.OCPolarity = LPTIM_OCPOLARITY_HIGH;
  if (HAL_LPTIM_OC_ConfigChannel(&hlptim2, &sConfig1, LPTIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN LPTIM2_Init 2 */

  /* USER CODE END LPTIM2_Init 2 */
  HAL_LPTIM_MspPostInit(&hlptim2);

}

/**
  * @brief RNG Initialization Function
  * @param None
  * @retval None
  */
static void MX_RNG_Init(void)
{

  /* USER CODE BEGIN RNG_Init 0 */

  /* USER CODE END RNG_Init 0 */

  /* USER CODE BEGIN RNG_Init 1 */

  /* USER CODE END RNG_Init 1 */
  hrng.Instance = RNG;
  hrng.Init.ClockErrorDetection = RNG_CED_ENABLE;
  if (HAL_RNG_Init(&hrng) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN RNG_Init 2 */

  /* USER CODE END RNG_Init 2 */

}

/**
  * @brief RTC Initialization Function
  * @param None
  * @retval None
  */
static void MX_RTC_Init(void)
{

  /* USER CODE BEGIN RTC_Init 0 */

  /* USER CODE END RTC_Init 0 */

  RTC_TimeTypeDef sTime = {0};
  RTC_DateTypeDef sDate = {0};
  RTC_AlarmTypeDef sAlarm = {0};

  /* USER CODE BEGIN RTC_Init 1 */

  /* USER CODE END RTC_Init 1 */

  /** Initialize RTC Only
  */
  hrtc.Instance = RTC;
  hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
  hrtc.Init.AsynchPrediv = 127;
  hrtc.Init.SynchPrediv = 255;
  hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
  hrtc.Init.OutPutRemap = RTC_OUTPUT_REMAP_NONE;
  hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
  hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
  hrtc.Init.OutPutPullUp = RTC_OUTPUT_PULLUP_NONE;
  hrtc.Init.BinMode = RTC_BINARY_NONE;
  if (HAL_RTC_Init(&hrtc) != HAL_OK)
  {
    Error_Handler();
  }

  /* USER CODE BEGIN Check_RTC_BKUP */
  const uint32_t rtc_magic = 0x32F2u;
  if (HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR0) != rtc_magic)
  {
    /* First-time init only */
    sTime.Hours = 0x0;
    sTime.Minutes = 0x0;
    sTime.Seconds = 0x0;
    sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    sTime.StoreOperation = RTC_STOREOPERATION_RESET;
    if (HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BCD) != HAL_OK)
    {
      Error_Handler();
    }
    sDate.WeekDay = RTC_WEEKDAY_MONDAY;
    sDate.Month = RTC_MONTH_JANUARY;
    sDate.Date = 0x1;
    sDate.Year = 0x0;

    if (HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BCD) != HAL_OK)
    {
      Error_Handler();
    }

    /* Enable the Alarm A */
    sAlarm.AlarmTime.Hours = 0x0;
    sAlarm.AlarmTime.Minutes = 0x0;
    sAlarm.AlarmTime.Seconds = 0x0;
    sAlarm.AlarmTime.SubSeconds = 0x0;
    sAlarm.AlarmMask = RTC_ALARMMASK_DATEWEEKDAY|RTC_ALARMMASK_SECONDS;
    sAlarm.AlarmSubSecondMask = RTC_ALARMSUBSECONDMASK_ALL;
    sAlarm.AlarmDateWeekDaySel = RTC_ALARMDATEWEEKDAYSEL_DATE;
    sAlarm.AlarmDateWeekDay = 0x1;
    sAlarm.Alarm = RTC_ALARM_A;
    if (HAL_RTC_SetAlarm_IT(&hrtc, &sAlarm, RTC_FORMAT_BCD) != HAL_OK)
    {
      Error_Handler();
    }

    HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, rtc_magic);
  }
  /* USER CODE END Check_RTC_BKUP */

  /* USER CODE BEGIN RTC_Init 2 */

  /* USER CODE END RTC_Init 2 */

}

/**
  * @brief SPI1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_SPI1_Init(void)
{

  /* USER CODE BEGIN SPI1_Init 0 */

  /* USER CODE END SPI1_Init 0 */

  /* USER CODE BEGIN SPI1_Init 1 */

  /* USER CODE END SPI1_Init 1 */
  /* SPI1 parameter configuration*/
  hspi1.Instance = SPI1;
  hspi1.Init.Mode = SPI_MODE_MASTER;
  hspi1.Init.Direction = SPI_DIRECTION_2LINES_RXONLY;
  hspi1.Init.DataSize = SPI_DATASIZE_16BIT;
  hspi1.Init.CLKPolarity = SPI_POLARITY_HIGH;
  hspi1.Init.CLKPhase = SPI_PHASE_1EDGE;
  hspi1.Init.NSS = SPI_NSS_SOFT;
  hspi1.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_16;
  hspi1.Init.FirstBit = SPI_FIRSTBIT_MSB;
  hspi1.Init.TIMode = SPI_TIMODE_DISABLE;
  hspi1.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
  hspi1.Init.CRCPolynomial = 7;
  hspi1.Init.CRCLength = SPI_CRC_LENGTH_DATASIZE;
  hspi1.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
  if (HAL_SPI_Init(&hspi1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SPI1_Init 2 */

  /* USER CODE END SPI1_Init 2 */

}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 0;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 59;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 0;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  if (HAL_TIM_PWM_ConfigChannel(&htim2, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */
  HAL_TIM_MspPostInit(&htim2);

}

/**
  * @brief USB Initialization Function
  * @param None
  * @retval None
  */
static void MX_USB_PCD_Init(void)
{

  /* USER CODE BEGIN USB_Init 0 */

  /* USER CODE END USB_Init 0 */

  /* USER CODE BEGIN USB_Init 1 */

  /* USER CODE END USB_Init 1 */
  hpcd_USB_DRD_FS.Instance = USB_DRD_FS;
  hpcd_USB_DRD_FS.Init.dev_endpoints = 8;
  hpcd_USB_DRD_FS.Init.speed = USBD_FS_SPEED;
  hpcd_USB_DRD_FS.Init.phy_itface = PCD_PHY_EMBEDDED;
  hpcd_USB_DRD_FS.Init.Sof_enable = DISABLE;
  hpcd_USB_DRD_FS.Init.low_power_enable = DISABLE;
  hpcd_USB_DRD_FS.Init.lpm_enable = DISABLE;
  hpcd_USB_DRD_FS.Init.battery_charging_enable = DISABLE;
  if (HAL_PCD_Init(&hpcd_USB_DRD_FS) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USB_Init 2 */
    HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS , 0x00 , PCD_SNG_BUF, 0x20); /* EP0 OUT */
    HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS , 0x80 , PCD_SNG_BUF, 0x60); /* EP0 IN */
    HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS , 0x81 , PCD_SNG_BUF, 0xA0); /* EP1 IN */
    HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS , 0x82 , PCD_SNG_BUF, 0xE0); /* EP2 IN */
    HAL_PCDEx_PMAConfig(&hpcd_USB_DRD_FS , 0x03 , PCD_SNG_BUF, 0xF0); /* EP3 OUT */

    /* NOTE: Do NOT start USB or init USBX DCD here.
     * It is done in MX_USBX_Device_Init() after USBX stack is ready.
     */
  /* USER CODE END USB_Init 2 */

}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{

  /* DMA controller clock enable */
  __HAL_RCC_DMA1_CLK_ENABLE();

  /* DMA interrupt init */
  /* DMA1_Channel1_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);
  /* DMA1_Channel2_3_IRQn interrupt configuration */
  HAL_NVIC_SetPriority(DMA1_Channel2_3_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Channel2_3_IRQn);

}

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
  __HAL_RCC_GPIOF_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, CTL_CEN_Pin|CTL_LEN_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : B1_Pin B2_Pin */
  GPIO_InitStruct.Pin = B1_Pin|B2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pin : USB_Pin */
  GPIO_InitStruct.Pin = USB_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING_FALLING;
  GPIO_InitStruct.Pull = GPIO_PULLDOWN;
  HAL_GPIO_Init(USB_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : LED_Pin */
  GPIO_InitStruct.Pin = LED_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : STA_CHG_Pin */
  GPIO_InitStruct.Pin = STA_CHG_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(STA_CHG_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pins : CTL_CEN_Pin CTL_LEN_Pin */
  GPIO_InitStruct.Pin = CTL_CEN_Pin|CTL_LEN_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : BL_Pin */
  GPIO_InitStruct.Pin = BL_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(BL_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */
  /* Clear any stale pending EXTI flags before enabling IRQ. */
  __HAL_GPIO_EXTI_CLEAR_IT(B1_Pin);
  __HAL_GPIO_EXTI_CLEAR_IT(B2_Pin);
  __HAL_GPIO_EXTI_CLEAR_IT(USB_Pin);
  if (!s_gpio_skip_exti_nvic)
  {
    HAL_NVIC_SetPriority(EXTI0_1_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(EXTI0_1_IRQn);
    HAL_NVIC_SetPriority(EXTI2_3_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(EXTI2_3_IRQn);
  }

  /* Default indicator LED to OFF. */
  IND_LED_Off();
  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */
uint8_t USB_IsPresent(void)
{
  return (HAL_GPIO_ReadPin(USB_GPIO_Port, USB_Pin) == GPIO_PIN_SET) ? 1u : 0u;
}

/* MiniPascal HAL glue (board integration). */
int mp_hal_getchar(void)
{
  return -1;
}

void mp_hal_putchar(char c)
{
  char b[2] = {c, 0};
  cdc_write_str(b);
}

uint32_t mp_hal_millis(void)
{
  return HAL_GetTick();
}

int mp_hal_abort_pressed(void)
{
  return (HAL_GPIO_ReadPin(B2_GPIO_Port, B2_Pin) == GPIO_PIN_SET) ? 1 : 0;
}

int mp_hal_usb_connected(void)
{
  return (USB_IsPresent() != 0u) ? 1 : 0;
}

void mp_hal_led_power_on(void)
{
  if (HAL_GPIO_ReadPin(CTL_LEN_GPIO_Port, CTL_LEN_Pin) == GPIO_PIN_RESET)
  {
    HAL_GPIO_WritePin(CTL_LEN_GPIO_Port, CTL_LEN_Pin, GPIO_PIN_SET);
    LP_DELAY(100);
  }
}

void mp_hal_led_power_off(void)
{
  HAL_GPIO_WritePin(CTL_LEN_GPIO_Port, CTL_LEN_Pin, GPIO_PIN_RESET);
}

/* Battery-only hold actions sampled while awake (SysTick runs only outside STOP2). */
#define BL_SLEEP_HOLD_MS 2000u
#define BL_ACTIVE_STATE GPIO_PIN_SET
static volatile uint32_t s_bl_hold_ms = 0u;
static volatile uint8_t s_bl_sleep_req = 0u;

#define B2_SHUTDOWN_HOLD_MS 2000u
#define B2_ACTIVE_STATE GPIO_PIN_SET
static uint32_t s_b2_hold_start_ms = 0u;

static void B2_Hold_Service_NoSleep(uint32_t now_ms)
{
  if (USB_IsPresent() != 0u)
  {
    s_b2_hold_start_ms = 0u;
    return;
  }

  if (HAL_GPIO_ReadPin(B2_GPIO_Port, B2_Pin) == B2_ACTIVE_STATE)
  {
    if (s_b2_hold_start_ms == 0u) s_b2_hold_start_ms = now_ms;
    if ((uint32_t)(now_ms - s_b2_hold_start_ms) >= B2_SHUTDOWN_HOLD_MS)
      EnterShutdown();
  }
  else
  {
    s_b2_hold_start_ms = 0u;
  }
}

static void B2_Hold_Service_Blocking(void)
{
  if (USB_IsPresent() != 0u)
  {
    s_b2_hold_start_ms = 0u;
    return;
  }

  if (HAL_GPIO_ReadPin(B2_GPIO_Port, B2_Pin) != B2_ACTIVE_STATE)
  {
    s_b2_hold_start_ms = 0u;
    return;
  }

  while (HAL_GPIO_ReadPin(B2_GPIO_Port, B2_Pin) == B2_ACTIVE_STATE)
  {
    B2_Hold_Service_NoSleep(HAL_GetTick());
    HAL_PWR_EnterSLEEPMode(PWR_LOWPOWERREGULATOR_ON, PWR_SLEEPENTRY_WFI);
  }

  s_b2_hold_start_ms = 0u;
}

static volatile uint8_t s_mp_wut_fired = 0u;
void HAL_RTCEx_WakeUpTimerEventCallback(RTC_HandleTypeDef *hrtc_ptr)
{
  (void)hrtc_ptr;
  s_mp_wut_fired = 1u;
}

static void lp_delay_sleep_ms(uint32_t ms)
{
  if (ms == 0u) return;
  uint32_t start = HAL_GetTick();
  while ((uint32_t)(HAL_GetTick() - start) < ms)
  {
    B2_Hold_Service_Blocking();
    HAL_PWR_EnterSLEEPMode(PWR_LOWPOWERREGULATOR_ON, PWR_SLEEPENTRY_WFI);
  }
}

static uint8_t lp_delay_rtc_ready(void)
{
  return (hrtc.Instance == RTC) ? 1u : 0u;
}

void LP_DELAY(uint32_t ms)
{
  if (ms == 0u) return;

  if (USB_IsPresent() != 0u)
  {
    HAL_Delay(ms);
    return;
  }

  if ((ms < 20u) || (lp_delay_rtc_ready() == 0u))
  {
    lp_delay_sleep_ms(ms);
    return;
  }

  /* RTC WUT @ RTCCLK/16 = 32768/16 = 2048 Hz (0.488 ms resolution), max ~32 s per shot. */
  while (ms)
  {
    uint32_t chunk_ms = ms;
    if (chunk_ms > 32000u) chunk_ms = 32000u;

    B2_Hold_Service_Blocking();
    uint32_t ticks = (chunk_ms * 2048u + 999u) / 1000u;
    if (ticks < 1u) ticks = 1u;
    if (ticks > 65536u) ticks = 65536u;

    s_mp_wut_fired = 0u;
    (void)HAL_RTCEx_DeactivateWakeUpTimer(&hrtc);
    __HAL_RTC_WAKEUPTIMER_CLEAR_FLAG(&hrtc, RTC_FLAG_WUTF);

    if (HAL_RTCEx_SetWakeUpTimer_IT(&hrtc, (uint32_t)(ticks - 1u), RTC_WAKEUPCLOCK_RTCCLK_DIV16, 0u) != HAL_OK)
    {
      /* Fallback to light sleep if WUT setup fails. */
      lp_delay_sleep_ms(chunk_ms);
    }
    else
    {
      while (!s_mp_wut_fired)
      {
        B2_Hold_Service_Blocking();
        __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WU);
        HAL_PWREx_EnterSTOP2Mode(PWR_STOPENTRY_WFI);
        SystemClock_Config();

        /* If we woke because of B2, stay awake in light sleep so HAL_GetTick() can count the 2s hold. */
        B2_Hold_Service_Blocking();
      }
      (void)HAL_RTCEx_DeactivateWakeUpTimer(&hrtc);
    }

    ms -= chunk_ms;
  }
}

void mp_hal_lowpower_delay_ms(uint32_t ms)
{
  LP_DELAY(ms);
}

/* RAM usage monitor (CLI MEM). */
extern void *_sbrk(ptrdiff_t incr);

static volatile uint8_t  s_memmon_inited = 0u;
static volatile uint32_t s_mem_heap_end = 0u;
static volatile uint32_t s_mem_min_free = 0xFFFFFFFFu;
static volatile uint32_t s_mem_min_tick_ms = 0u;
static volatile uint8_t  s_mem_min_need_ts = 0u;
static char s_mem_min_dt[RTC_DATETIME_STRING_SIZE] = "N/A";

static uint32_t memmon_total_bytes(void)
{
  return (uint32_t)(&_estack) - RAM_START_ADDR;
}

static uint32_t memmon_free_bytes_with_heap_end(uint32_t heap_end)
{
  uint32_t msp = (uint32_t)__get_MSP();
  if (msp <= heap_end) return 0u;
  return msp - heap_end;
}

static void memmon_update_min(uint32_t free_now, uint32_t tick_ms)
{
  if (free_now < s_mem_min_free)
  {
    s_mem_min_free = free_now;
    s_mem_min_tick_ms = tick_ms;
    s_mem_min_need_ts = 1u;
  }
}

void MemMon_Init(void)
{
  void *p = _sbrk(0);
  if (p == (void *)-1) p = 0;
  s_mem_heap_end = (uint32_t)p;

  s_mem_min_free = 0xFFFFFFFFu;
  s_mem_min_tick_ms = HAL_GetTick();
  s_mem_min_need_ts = 1u;
  (void)memset(s_mem_min_dt, 0, sizeof(s_mem_min_dt));
  (void)strncpy(s_mem_min_dt, "N/A", sizeof(s_mem_min_dt) - 1u);

  s_memmon_inited = 1u;
  memmon_update_min(memmon_free_bytes_with_heap_end(s_mem_heap_end), HAL_GetTick());
}

void MemMon_Task(void)
{
  if (!s_memmon_inited) return;

  void *p = _sbrk(0);
  if (p != (void *)-1)
    s_mem_heap_end = (uint32_t)p;

  memmon_update_min(memmon_free_bytes_with_heap_end(s_mem_heap_end), HAL_GetTick());

  if (s_mem_min_need_ts)
  {
    char dt[RTC_DATETIME_STRING_SIZE];
    if (RTC_ReadClock(dt) == HAL_OK)
    {
      (void)strncpy(s_mem_min_dt, dt, sizeof(s_mem_min_dt) - 1u);
      s_mem_min_dt[sizeof(s_mem_min_dt) - 1u] = '\0';
    }
    else
    {
      (void)strncpy(s_mem_min_dt, "RTC_ERR", sizeof(s_mem_min_dt) - 1u);
      s_mem_min_dt[sizeof(s_mem_min_dt) - 1u] = '\0';
    }
    s_mem_min_need_ts = 0u;
  }
}

void MemMon_TickHook(void)
{
  if (!s_memmon_inited) return;
  memmon_update_min(memmon_free_bytes_with_heap_end(s_mem_heap_end), HAL_GetTick());
}

void MemMon_Get(uint32_t *out_total, uint32_t *out_free, uint32_t *out_min_free,
                uint32_t *out_min_tick_ms, char *min_dt, uint32_t min_dt_sz)
{
  if (out_total) *out_total = memmon_total_bytes();

  uint32_t heap_end = s_mem_heap_end;
  uint32_t free_now = 0u;
  void *p = _sbrk(0);
  if (p != (void *)-1)
    heap_end = (uint32_t)p;
  free_now = memmon_free_bytes_with_heap_end(heap_end);

  if (out_free) *out_free = free_now;

  uint32_t now = HAL_GetTick();
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  s_mem_heap_end = heap_end;
  memmon_update_min(free_now, now);
  uint32_t min_free = s_mem_min_free;
  uint32_t min_tick = s_mem_min_tick_ms;
  char dt_copy[RTC_DATETIME_STRING_SIZE];
  (void)memset(dt_copy, 0, sizeof(dt_copy));
  (void)strncpy(dt_copy, s_mem_min_dt, sizeof(dt_copy) - 1u);
  __set_PRIMASK(primask);

  if (out_min_free) *out_min_free = min_free;
  if (out_min_tick_ms) *out_min_tick_ms = min_tick;

  if (min_dt && (min_dt_sz != 0u))
  {
    (void)strncpy(min_dt, dt_copy, min_dt_sz - 1u);
    min_dt[min_dt_sz - 1u] = '\0';
  }
}

static void Power_MinimizeLoads(void)
{
    mp_request_stop();

    /* Try to switch off the LED strip gracefully before cutting its power. */
    led_set_all_RGBW(0u, 0u, 0u, 0u);
    led_render();

    HAL_GPIO_WritePin(CTL_LEN_GPIO_Port, CTL_LEN_Pin, GPIO_PIN_RESET);
    IND_LED_Off();
}

/* Return 1 if B1 stays pressed for hold_ms after wake, else 0. */
#define B1_WAKE_HOLD_MS   2000u
#define B1_ACTIVE_STATE   GPIO_PIN_SET
static uint8_t B1_WaitHeld(uint32_t hold_ms)
{
    if (HAL_GPIO_ReadPin(B1_GPIO_Port, B1_Pin) != B1_ACTIVE_STATE)
        return 0u;

    uint32_t start = HAL_GetTick();
    while ((uint32_t)(HAL_GetTick() - start) < hold_ms)
    {
        if (HAL_GPIO_ReadPin(B1_GPIO_Port, B1_Pin) != B1_ACTIVE_STATE)
            return 0u;
        HAL_PWR_EnterSLEEPMode(PWR_LOWPOWERREGULATOR_ON, PWR_SLEEPENTRY_WFI);
    }
    return 1u;
}

static void EnterStop(void)
{
    while (1)
    {
        Power_MinimizeLoads();

        /* If B1 is already held (e.g., wake from shutdown), accept it without entering STOP2. */
        if (B1_WaitHeld(B1_WAKE_HOLD_MS))
        {
            s_battery_run_allowed = 1u;
            mp_request_run_loaded();
            return;
        }

        /* If B2 is held, allow shutdown request while we are about to sleep. */
        B2_Hold_Service_Blocking();

        /* Arm STOP2 wake tracking and clear any stale EXTI flags (prevents immediate wake). */
        s_stop2_armed = 1u;
        s_stop2_woke_by_b1 = 0u;
        __HAL_GPIO_EXTI_CLEAR_IT(B1_Pin);
        __HAL_GPIO_EXTI_CLEAR_IT(B2_Pin);
        __HAL_GPIO_EXTI_CLEAR_IT(USB_Pin);

        /* Clear wakeup flag and enter STOP2 mode (wake via EXTI). */
        __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WU);
        HAL_PWREx_EnterSTOP2Mode(PWR_STOPENTRY_WFI);

        /* Reconfigure system clock after STOP2 wakeup. */
        SystemClock_Config();

        s_stop2_armed = 0u;
        uint8_t woke_by_b1 = s_stop2_woke_by_b1;
        if (!woke_by_b1 && (HAL_GPIO_ReadPin(B1_GPIO_Port, B1_Pin) == B1_ACTIVE_STATE))
            woke_by_b1 = 1u;

        if (woke_by_b1)
        {
            s_stop2_woke_by_b1 = 0u;
            if (B1_WaitHeld(B1_WAKE_HOLD_MS))
            {
                s_battery_run_allowed = 1u;
                mp_request_run_loaded();
                return;
            }
            /* Short/false wake: go back to STOP2. */
            continue;
        }

        /* If woke due to B2, wait for 2s hold and then shutdown. */
        B2_Hold_Service_Blocking();

        /* Wake by something else: return to caller. (USB attach will reset anyway.) */
        return;
    }
}

static void EnterShutdown(void)
{
    /* Force-stop any program and blank LEDs before powering down. */
    mp_force_stop();
    Power_MinimizeLoads();

    /* On battery, also disable charger enable. */
    HAL_GPIO_WritePin(CTL_CEN_GPIO_Port, CTL_CEN_Pin, GPIO_PIN_RESET);

    /* Configure wake sources: only B1 (WKUP1, PA0) wakes the MCU. */
    HAL_SuspendTick();
    HAL_PWR_DisableWakeUpPin(PWR_WAKEUP_PIN1_HIGH);
    HAL_PWR_DisableWakeUpPin(PWR_WAKEUP_PIN2_HIGH);
    HAL_PWR_DisableWakeUpPin(PWR_WAKEUP_PIN3_HIGH);
    HAL_PWR_DisableWakeUpPin(PWR_WAKEUP_PIN4_HIGH);
    HAL_PWR_DisableWakeUpPin(PWR_WAKEUP_PIN5_HIGH);
    HAL_PWR_DisableWakeUpPin(PWR_WAKEUP_PIN7_HIGH);

    __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WU);
    HAL_PWR_EnableWakeUpPin(PWR_WAKEUP_PIN1_HIGH);

    /* Enter deepest low power mode (wake causes reset). */
    HAL_PWR_EnterSHUTDOWNMode();
    while (1) { }
}

static volatile uint8_t s_lamp_off_req = 0u;
static volatile uint8_t s_lamp_off_stop2 = 0u;

void Lamp_RequestOff(uint8_t enter_stop2)
{
    s_lamp_off_req = 1u;
    s_lamp_off_stop2 = (enter_stop2 != 0u) ? 1u : 0u;
}

static void Lamp_Off_Task(void)
{
    if (!s_lamp_off_req) return;
    s_lamp_off_req = 0u;

    if (s_lamp_off_stop2)
    {
        s_lamp_off_stop2 = 0u;
        EnterStop(); /* Includes Power_MinimizeLoads() */
        return;
    }

    Power_MinimizeLoads();
}

static void NoProgram_SleepUntilUSB(void)
{
    /* Blink 3x so user sees "no program" state (200ms on/off). */
    for (uint8_t i = 0; i < 3; i++)
    {
        IND_LED_On();
        LP_DELAY(200);
        IND_LED_Off();
        LP_DELAY(200);
    }

    /* Disable button EXTI lines so only USB connect can wake us. */
    uint32_t imr1 = EXTI->IMR1;
    EXTI->IMR1 = (imr1 & ~(B1_Pin | B2_Pin)) | USB_Pin;
    __HAL_GPIO_EXTI_CLEAR_IT(B1_Pin);
    __HAL_GPIO_EXTI_CLEAR_IT(B2_Pin);
    __HAL_GPIO_EXTI_CLEAR_IT(USB_Pin);

    /* Wait for USB to be connected. */
    HAL_SuspendTick();
    while (USB_IsPresent() == 0u)
    {
        Power_MinimizeLoads();
        __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WU);
        HAL_PWR_EnterSTOPMode(PWR_LOWPOWERREGULATOR_ON, PWR_STOPENTRY_WFI);
    }
    HAL_ResumeTick();

    /* Restore EXTI masks (in practice USB attach will trigger reset). */
    EXTI->IMR1 = imr1;
}

static void BL_Task(void)
{
    if (s_bl_sleep_req)
    {
        s_bl_sleep_req = 0u;
        EnterStop();
    }
    Lamp_Off_Task();
}

void HAL_SYSTICK_Callback(void)
{
    if (USB_IsPresent() != 0u)
    {
        s_bl_hold_ms = 0u;
        s_bl_sleep_req = 0u;
    }
    else if (HAL_GPIO_ReadPin(BL_GPIO_Port, BL_Pin) == BL_ACTIVE_STATE)
    {
        if (s_bl_hold_ms < BL_SLEEP_HOLD_MS) s_bl_hold_ms++;
        if (s_bl_hold_ms >= BL_SLEEP_HOLD_MS) s_bl_sleep_req = 1u;
    }
    else
    {
        s_bl_hold_ms = 0u;
        s_bl_sleep_req = 0u;
    }

    MemMon_TickHook();
}

void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    static uint8_t usb_reset_done = 0;

    if (GPIO_Pin == B1_Pin)
    {
        if (s_stop2_armed)
        {
            /* Short BT1 press should start lamp after STOP2 wake. */
            s_stop2_woke_by_b1 = 1u;
        }
    }

    if (GPIO_Pin == USB_Pin)
    {
        if (USB_IsPresent() != 0u)
        {
            if (!usb_reset_done)
            {
                usb_reset_done = 1;
                for (volatile uint32_t i = 0; i < 100000; i++) { }
                NVIC_SystemReset();
            }
        }
        else
        {
            usb_reset_done = 0;
            USB_CLI_NotifyDetach();
            /* USB disconnected: switch to battery/autorun mode. */
            mp_request_usb_detach();
            mp_force_stop();
            /* Ensure indicator LED is not left stuck in "charger mirror" state. */
            IND_LED_Off();
            /* Do not autorun on detach: go to STOP2 and wait for B1 hold. */
            s_battery_run_allowed = 0u;
            Lamp_RequestOff(1u);
        }
    }
}



/**
 * @brief Jump to STM32 system bootloader
 * @note Deinitializes all peripherals and jumps to system memory bootloader
 *       STM32U073: System memory 26KB at 0x1FFF0000 (from AN2606)
 */
static void JumpToBootloader(void)
{
    /* STM32U073 system memory bootloader address (AN2606 section 86) */
    #define BOOT_ADD 0x1FFF0000UL
    
    /* Visual indication - blink LED 2x before jumping */
    for (int i = 0; i < 2; i++)
    {
        IND_LED_On();
        LP_DELAY(100);
        IND_LED_Off();
        LP_DELAY(100);
    }
    
    /* 1. Disable all interrupts */
    __disable_irq();

    /* 2. Disable HAL tick (SysTick) */
    SysTick->CTRL = 0;
    SysTick->LOAD = 0;
    SysTick->VAL = 0;
    
    /* 3. Disable and clear all NVIC interrupt bits */
    for (uint8_t i = 0; i < sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0]); i++)
    {
        NVIC->ICER[i] = 0xFFFFFFFF;
        NVIC->ICPR[i] = 0xFFFFFFFF;
    }

    /* 4. Clear EXTI pending flags and mask EXTI lines (avoid spurious IRQs in ROM bootloader). */
    __HAL_GPIO_EXTI_CLEAR_IT(B1_Pin);
    __HAL_GPIO_EXTI_CLEAR_IT(B2_Pin);
    __HAL_GPIO_EXTI_CLEAR_IT(USB_Pin);
    EXTI->IMR1 = 0u;

    /* 5. Reset clocks and HAL state back to defaults (HSI, no PLL, peripherals reset). */
    (void)HAL_DeInit();
    (void)HAL_RCC_DeInit();
    
    /* 6. Enable SYSCFG clock for memory remap */
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    
    /* 7. Remap system memory to 0x00000000 - MUST be before MSP/jump */
    __HAL_SYSCFG_REMAPMEMORY_SYSTEMFLASH();
    
    /* 8. Read bootloader stack pointer and reset vector */
    uint32_t bootloader_stack = *(__IO uint32_t *)(BOOT_ADD);
    uint32_t bootloader_reset = *(__IO uint32_t *)(BOOT_ADD + 4);
    
    /* 9. Set the main stack pointer */
    __set_MSP(bootloader_stack);

#if defined(SCB) && defined(SCB_VTOR_TBLOFF_Msk)
    /* On Cortex-M0+, also point VTOR to system memory for safety. */
    SCB->VTOR = BOOT_ADD;
#endif
    __DSB();
    __ISB();

    /* 10. Jump to bootloader reset handler (keep IRQs disabled) */
    void (*jump_to_boot)(void) = (void (*)(void))(bootloader_reset);
    jump_to_boot();
    
    /* Should never reach here */
    while (1);
}

/**
 * @brief Check if BL button is pressed at startup and jump to bootloader
 * @note Must be called after MX_GPIO_Init()
 *       BL is on PF3 (EXTI3), which is shared with USB detect on PA3 (EXTI3).
 *       To avoid EXTI source conflicts, BL is sampled by polling only.
 *       - Button pressed: blocks here (no other peripherals init)
 *       - Released before 5s: continues normal boot
 *       - Held for 5s: jumps to DFU bootloader (system memory)
 */
static void CheckBootloaderEntry(void)
{
    /* BL_Pin already initialized by MX_GPIO_Init() */
    
    /* If button not pressed, continue immediately */
    if (HAL_GPIO_ReadPin(BL_GPIO_Port, BL_Pin) != BL_ACTIVE_STATE)
    {
        return;
    }
    
    /* Button is pressed - block here and wait */
    uint32_t start_tick = HAL_GetTick();
    
    while (1)
    {
        /* Button released - continue normal boot */
        if (HAL_GPIO_ReadPin(BL_GPIO_Port, BL_Pin) != BL_ACTIVE_STATE)
        {
            return;  /* Exit and continue with peripheral initialization */
        }
        
        /* Button held for 5 seconds - jump to bootloader */
        if ((HAL_GetTick() - start_tick) >= 5000u)
        {
            JumpToBootloader();
            /* Never returns */
        }
        
        LP_DELAY(10); /* Check every 10ms for faster response */
    }
}

static void rtc_wakeup_1s_enable(void)
{
    static uint8_t nvic_init = 0;
    if (!nvic_init)
    {
        /* STM32U073: Wakeup/Alarm share RTC_TAMP_IRQn */
        HAL_NVIC_SetPriority(RTC_TAMP_IRQn, 0, 0);
        HAL_NVIC_EnableIRQ(RTC_TAMP_IRQn);
        nvic_init = 1;
    }

    (void)HAL_RTCEx_DeactivateWakeUpTimer(&hrtc);
    (void)HAL_RTCEx_SetWakeUpTimer_IT(&hrtc, 0, RTC_WAKEUPCLOCK_CK_SPRE_16BITS, 0);
}

static void rtc_wakeup_disable(void)
{
    (void)HAL_RTCEx_DeactivateWakeUpTimer(&hrtc);
}

static float vbat_read_blocking(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    uint32_t id0 = ANALOG_GetUpdateId();
    ANALOG_RequestUpdate();
    while ((HAL_GetTick() - start) < timeout_ms)
    {
        ANALOG_Task();
        if (ANALOG_GetUpdateId() != id0)
            break;
        __WFI();
    }
    return ANALOG_GetBat();
}

#define LOWBAT_MAGIC 0xB007u
#define LOWBAT_BKP_REG RTC_BKP_DR1

static void EnterLowBatteryStandby(void)
{
    if (USB_IsPresent() != 0u)
        return;

    rtc_wakeup_1s_enable();

    /* Minimize loads */
    Power_MinimizeLoads();

    /* Standby with RTC wakeup: lowest power, wake causes reset */
    __HAL_PWR_CLEAR_FLAG(PWR_FLAG_WU);
    __HAL_PWR_CLEAR_FLAG(PWR_FLAG_SB);
    HAL_SuspendTick();
    HAL_PWR_EnterSTANDBYMode();
}

static void LowBattery_EarlyGate(void)
{
    if (USB_IsPresent() != 0u)
    {
        HAL_RTCEx_BKUPWrite(&hrtc, LOWBAT_BKP_REG, 0u);
        rtc_wakeup_disable();
        return;
    }

    uint32_t magic = HAL_RTCEx_BKUPRead(&hrtc, LOWBAT_BKP_REG);
    float vbat = vbat_read_blocking(50);

    if (magic == LOWBAT_MAGIC)
    {
        if (vbat < CHARGER_VBAT_RECOVERY)
        {
            EnterLowBatteryStandby();
        }
        HAL_RTCEx_BKUPWrite(&hrtc, LOWBAT_BKP_REG, 0u);
        rtc_wakeup_disable();
        return;
    }

    if (vbat < CHARGER_VBAT_CRITICAL)
    {
        HAL_RTCEx_BKUPWrite(&hrtc, LOWBAT_BKP_REG, LOWBAT_MAGIC);
        EnterLowBatteryStandby();
    }
}

static void LowBattery_Task(void)
{
    static uint32_t last_check_ms = 0;
    if (USB_IsPresent() != 0u)
        return;

    uint32_t now = HAL_GetTick();
    if ((now - last_check_ms) < 1000u)
        return;
    last_check_ms = now;

    float vbat = vbat_read_blocking(50);
    if (vbat < CHARGER_VBAT_CRITICAL)
    {
        HAL_RTCEx_BKUPWrite(&hrtc, LOWBAT_BKP_REG, LOWBAT_MAGIC);
        EnterLowBatteryStandby();
    }
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
