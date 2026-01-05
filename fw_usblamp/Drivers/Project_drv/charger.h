/*
 * charger.h - Battery charger management for STNS01
 *
 *  Created on: Jan 2, 2026
 *      Author: orgo
 * 
 *  Controls STNS01 battery charger with voltage monitoring and sleep mode
 */

#ifndef PROJECT_DRV_CHARGER_H_
#define PROJECT_DRV_CHARGER_H_

#include "stm32u0xx_hal.h"
#include <stdint.h>

/* Voltage thresholds for charge control (in volts) */
#define CHARGER_VBAT_STOP       4.1f    /* Stop charging above this voltage */
#define CHARGER_VBAT_START      3.8f    /* Start charging below this voltage */
#define CHARGER_VBAT_CRITICAL   2.9f    /* Critical low - disconnect USB */
#define CHARGER_VBAT_RECOVERY   3.1f    /* Recovery threshold - reset MCU if USB connected */

/**
 * @brief Initialize charger driver
 * @note Call once during system init, after GPIO and ADC init
 */
void CHARGER_Init(void);

/**
 * @brief Charger task - call periodically from main loop
 * @note Monitors battery voltage and controls charging
 *       Updates LED to mirror STA_CHG pin status
 */
void CHARGER_Task(void);

/**
 * @brief Get charging status
 * @return 1 if charging enabled, 0 if disabled
 */
uint8_t CHARGER_IsCharging(void);

/**
 * @brief Get STA_CHG pin status
 * @return 1=charging, 2=not charging, 3=error (toggling/fault)
 */
uint8_t CHARGER_GetStatus(void);

/**
 * @brief Reset charger by toggling CTL_CEN pin
 * @note Use this to clear fault conditions (timeout, VPRE error, end-of-charge)
 */
void CHARGER_Reset(void);

#endif /* PROJECT_DRV_CHARGER_H_ */
