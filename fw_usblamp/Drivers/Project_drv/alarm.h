/*
 * alarm.h - simple beeper driver
 */

#ifndef PROJECT_DRV_ALARM_H_
#define PROJECT_DRV_ALARM_H_

#include "stm32u0xx_hal.h"
#include <stdint.h>

/**
 * @brief Start beep tone on LPTIM2 CH1.
 *
 * Non-blocking: returns immediately; auto-stops after the requested duration.
 *
 * @param freq_hz Tone frequency in Hz
 * @param volume Duty-cycle in percent (0..50). 50% is max loudness.
 * @param time_s Duration in seconds
 */
void BEEP(uint16_t freq_hz, uint8_t volume, float time_s);

/* Call periodically from main loop to run any deferred BEEP() request made from ISR context. */
void BEEP_Task(void);

/* Debug helper: starts 1 kHz, 50% PWM on PA4 without interrupts (no auto-stop). */
void BEEP_Test1kHz(void);

#endif /* PROJECT_DRV_ALARM_H_ */
