/*
 * analog.h - Analog measurements (light sensor, battery, VCC).
 */

#ifndef PROJECT_DRV_ANALOG_H_
#define PROJECT_DRV_ANALOG_H_

#include "stm32u0xx_hal.h"

/* Configuration constants */
#define ANALOG_VREF             3.3f        /* Reference voltage (nominal). */
#define ANALOG_ADC_MAX_VALUE    4095.0f     /* 12-bit without oversampling. */

/* Light sensor (TIA with SFH203P photodiode) configuration */
#define ANALOG_TIA_RESISTOR     330000.0f       /* 330k ohm TIA feedback resistor. */
#define ANALOG_LIGHT_CHANNEL    ADC_CHANNEL_14  /* PA7 - AN_LI */

/*
 * Photodiode current-to-lux conversion.
 *
 * Default is an estimate for SFH203P around photopic peak (approx.).
 * If you need accuracy, calibrate this constant with a luxmeter.
 *
 * Units: A / lux.
 */
#ifndef ANALOG_PD_A_PER_LUX
#define ANALOG_PD_A_PER_LUX     (3.84e-9f)
#endif

/* Battery voltage divider configuration */
#define ANALOG_BAT_R1           100000.0f       /* 100k ohm (top resistor). */
#define ANALOG_BAT_R2           47000.0f        /* 47k ohm (bottom resistor). */
#define ANALOG_BAT_DIVIDER      ((ANALOG_BAT_R2) / (ANALOG_BAT_R1 + ANALOG_BAT_R2))
#define ANALOG_BAT_CHANNEL      ADC_CHANNEL_17  /* PB0 - AN_BAT (ADC1_IN17 on QFN32). */

/* Number of ADC samples to average */
#define ANALOG_NUM_SAMPLES      10

/* Background auto-update period (ms); set to 0 to disable. */
#ifndef ANALOG_AUTO_UPDATE_MS
#define ANALOG_AUTO_UPDATE_MS   1000U
#endif

/* Public function prototypes */
void ANALOG_Init(ADC_HandleTypeDef *hadc);
void ANALOG_Task(void);
void ANALOG_RequestUpdate(void);
uint8_t ANALOG_IsBusy(void);
uint32_t ANALOG_GetUpdateId(void);

/* Returns last computed values (auto-refreshed in background). */
float ANALOG_GetLight(void);
float ANALOG_GetBat(void);
float ANALOG_GetVcc(void);

#endif /* PROJECT_DRV_ANALOG_H_ */
