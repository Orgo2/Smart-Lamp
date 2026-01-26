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

/*
 * Safety: do not start charging if VBAT is below this voltage.
 * (Manual one-shot override is available via CHARGER_LowBattEnableOnce()).
 */
#ifndef CHARGER_VBAT_MIN_START
#define CHARGER_VBAT_MIN_START  1.7f
#endif

/* Periodic task configuration (ms). */
#ifndef CHARGER_CHECK_INTERVAL_MS
#define CHARGER_CHECK_INTERVAL_MS  250u
#endif
#ifndef CHARGER_STA_SAMPLE_MS
#define CHARGER_STA_SAMPLE_MS      500u
#endif
#ifndef CHARGER_STA_WINDOW_MS
#define CHARGER_STA_WINDOW_MS      2000u
#endif

/* Initialize charger driver (call once during init, after GPIO and ADC init). */
void CHARGER_Init(void);

/* Charger task (call periodically). */
void CHARGER_Task(void);

/* Return 1 if charging enabled, 0 if disabled. */
uint8_t CHARGER_IsCharging(void);

/* Read charger status: 0=disabled/no USB, 1=charging, 2=not charging, 3=error/fault. */
uint8_t CHARGER_GetStatus(void);

/* Reset charger by toggling CTL_CEN (clears typical fault conditions). */
void CHARGER_Reset(void);

/* Allow one-time start of charging below CHARGER_VBAT_MIN_START (manual override). */
void CHARGER_LowBattEnableOnce(void);

/* USB CLI helper: print charger/battery status. */
typedef void (*charger_write_fn_t)(const char *s);
void CHARGER_WriteStatus(charger_write_fn_t write);

#endif /* PROJECT_DRV_CHARGER_H_ */
