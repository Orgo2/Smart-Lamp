/*
 * rtc.h - RTC helpers (time/date + alarm trigger flag).
 */

#ifndef PROJECT_DRV_RTC_H_
#define PROJECT_DRV_RTC_H_

#include "stm32u0xx_hal.h"
#include <stdint.h>
#include <stdbool.h>

/* DateTime string format: "HH:MM:SS_RR.MM.DD" (24-hour format). */
#define RTC_DATETIME_STRING_SIZE 24  /* 17 chars + NUL terminator + safety margin */

/* Alarm time string format: "HH:MM:SS" (24-hour format). */
#define RTC_ALARM_STRING_SIZE 10  /* 8 chars + NUL terminator + safety */

/* Initialize RTC (CubeMX config). */
HAL_StatusTypeDef RTC_Init(void);

/* Read current date/time into datetime_str (see RTC_DATETIME_STRING_SIZE). */
HAL_StatusTypeDef RTC_ReadClock(char *datetime_str);

/* Set RTC date/time from a formatted string (see RTC_DATETIME_STRING_SIZE). */
HAL_StatusTypeDef RTC_SetClock(const char *datetime_str);

/* Set alarm (alarm_str "HH:MM:SS" or "0", duration_sec seconds, callback_interval_sec ignored). */
HAL_StatusTypeDef RTC_SetAlarm(const char *alarm_str, uint8_t duration_sec, uint8_t callback_interval_sec);

/* Daily alarm helper (HH:MM only, seconds auto-calculated) */
HAL_StatusTypeDef RTC_SetDailyAlarm(uint8_t hh, uint8_t mm, uint8_t duration_sec);
HAL_StatusTypeDef RTC_GetDailyAlarm(uint8_t *hh, uint8_t *mm, uint8_t *duration_sec);

/* Alarm active flag (1 when alarm beeping/running, 0 otherwise) */
extern volatile uint8_t RTC_AlarmTrigger;

#endif /* PROJECT_DRV_RTC_H_ */
