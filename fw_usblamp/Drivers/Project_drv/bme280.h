/*
 * bme280.h
 */

#ifndef PROJECT_DRV_BME280_H_
#define PROJECT_DRV_BME280_H_

#include "stm32u0xx_hal.h"
#include <stdint.h>

#define BME280_I2C_ADDR  0x76  // CSB pin connected to GND

typedef struct {
    float temperature;
    float pressure;
    float humidity;
} BME280_Data_t;

// Inicializácia a deinicializácia (voliteľné, meracie funkcie ich volajú automaticky)
HAL_StatusTypeDef BME280_Init(I2C_HandleTypeDef *hi2c);
HAL_StatusTypeDef BME280_Deinit(void);

// Meracie funkcie (automaticky inicializujú a deinicializujú senzor)
HAL_StatusTypeDef RH(float *humidity);        // Vlhkosť (%)
HAL_StatusTypeDef T(float *temperature);      // Teplota (°C)
HAL_StatusTypeDef P(float *pressure);         // Tlak (hPa)
HAL_StatusTypeDef BME280(BME280_Data_t *data); // Všetky údaje

#endif /* PROJECT_DRV_BME280_H_ */
