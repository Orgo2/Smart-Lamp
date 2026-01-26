/*
 * alarm.h - simple beeper driver
 */

#ifndef PROJECT_DRV_ALARM_H_
#define PROJECT_DRV_ALARM_H_

#include "stm32u0xx_hal.h"
#include <stdint.h>

/* Start a beep on LPTIM2 CH1 (freq_hz Hz, volume 0..50%, time_s seconds). */
void BEEP(uint16_t freq_hz, uint8_t volume, float time_s);

/* Call periodically to run any deferred BEEP() request made from ISR context. */
void BEEP_Task(void);

/* Returns 1 while the beeper is active (use this to avoid STOP2 while beeping). */
uint8_t BEEP_IsActive(void);

#endif /* PROJECT_DRV_ALARM_H_ */
