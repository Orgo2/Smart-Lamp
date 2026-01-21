/*
 * charger.h - STNS01 charger control and status helpers.
 */

#ifndef PROJECT_DRV_CHARGER_H_
#define PROJECT_DRV_CHARGER_H_

#include "stm32u0xx_hal.h"
#include <stdint.h>

/* Voltage thresholds for charge control (in volts) */
#define CHARGER_VBAT_STOP       4.1f    /* Stop charging above this voltage */
#define CHARGER_VBAT_START      3.8f    /* Start charging below this voltage */
#define CHARGER_VBAT_CRITICAL   2.9f    /* Criticaly low, use the lowest rtc standby */
#define CHARGER_VBAT_RECOVERY   3.1f    /* Recovery threshold - reset MCU if USB connected */

/* Initialize charger driver (call once during init, after GPIO and ADC init). */
void CHARGER_Init(void);

/* Charger task (call periodically). */
void CHARGER_Task(void);

/* Return 1 if charging enabled, 0 if disabled. */
uint8_t CHARGER_IsCharging(void);

/* Read STA_CHG status: 1=charging, 2=not charging, 3=error/fault. */
uint8_t CHARGER_GetStatus(void);

/* Reset charger by toggling CTL_CEN (clears typical fault conditions). */
void CHARGER_Reset(void);

#endif /* PROJECT_DRV_CHARGER_H_ */
