/*
 * rtc.h
 *
 *  Created on: Dec 13, 2025
 *      Author: orgo
 * 
 *  RTC driver for STM32U073 using HAL from CubeMX configuration.
 *  Uses external hrtc handle initialized in main.c
 */

#ifndef PROJECT_DRV_RTC_H_
#define PROJECT_DRV_RTC_H_

#include "stm32u0xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

// DateTime string format: "HH:MM:SS_RR.MM.DD" (24-hour format)
#define RTC_DATETIME_STRING_SIZE 24  // 17 chars + null terminator + safety margin

// Alarm time string format: "HH:MM:SS" (24-hour format)
#define RTC_ALARM_STRING_SIZE 10  // 8 chars + null terminator + safety

/**
 * @brief Initialize RTC with external LSE crystal
 * @return HAL_OK on success, HAL_ERROR on failure
 */
HAL_StatusTypeDef RTC_Init(void);

/**
 * @brief Read current date and time from RTC
 * @param datetime_str Buffer to store formatted string "HH:MM:SS_RR.MM.DD"
 *                     Must be at least RTC_DATETIME_STRING_SIZE bytes
 * @return HAL_OK on success, HAL_ERROR on failure
 */
HAL_StatusTypeDef RTC_ReadClock(char *datetime_str);

/**
 * @brief Set RTC date and time
 * @param datetime_str Formatted string "HH:MM:SS_RR.MM.DD" (24-hour format)
 *                     HH: hours (00-23)
 *                     MM: minutes (00-59)
 *                     SS: seconds (00-59)
 *                     RR: year (00-99, represents 2000-2099)
 *                     MM: month (01-12)
 *                     DD: day (01-31)
 * @return HAL_OK on success, HAL_ERROR on failure
 */
HAL_StatusTypeDef RTC_SetClock(const char *datetime_str);

/**
 * @brief Set RTC alarm with duration (trigger-only, no callbacks)
 * @param alarm_str Formatted string "HH:MM:SS" or "0" to disable alarm
 *                  HH: hours (00-23)
 *                  MM: minutes (00-59)
 *                  SS: seconds (00-59)
 * @param duration_sec Alarm duration in seconds (1-255), 0 disables alarm
 * @param callback_interval_sec Ignored (callbacks removed, kept for API compatibility)
 * @return HAL_OK on success, HAL_ERROR on failure
 * 
 * Sets RTC_AlarmTrigger=1 when alarm fires, holds for duration_sec, then resets to 0
 * Example: RTC_SetAlarm("07:00:00", 120, 1) - alarm at 7:00 for 120s
 *          RTC_SetAlarm("0", 0, 0) - disable alarm
 */
HAL_StatusTypeDef RTC_SetAlarm(const char *alarm_str, uint8_t duration_sec, uint8_t callback_interval_sec);

/* Daily alarm helper (HH:MM only, seconds auto-calculated) */
HAL_StatusTypeDef RTC_SetDailyAlarm(uint8_t hh, uint8_t mm, uint8_t duration_sec);
HAL_StatusTypeDef RTC_GetDailyAlarm(uint8_t *hh, uint8_t *mm, uint8_t *duration_sec);

/* Alarm active flag (1 when alarm beeping/running, 0 otherwise) */
extern volatile uint8_t RTC_AlarmTrigger;

#endif /* PROJECT_DRV_RTC_H_ */
