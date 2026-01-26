/*
 * bme280.h - BME280 sensor helper (I2C).
 */

#ifndef PROJECT_DRV_BME280_H_
#define PROJECT_DRV_BME280_H_

#include "stm32u0xx_hal.h"
#include <stdint.h>

#define BME280_I2C_ADDR  0x76  /* CSB pin connected to GND */

/* Measurement/init timeouts (ms). */
#ifndef BME280_TIMEOUT_MS
#define BME280_TIMEOUT_MS       5000u
#endif
#ifndef BME280_I2C_TIMEOUT_MS
#define BME280_I2C_TIMEOUT_MS   500u   /* I2C operation timeout */
#endif

typedef struct {
    float temperature;
    float pressure;
    float humidity;
} BME280_Data_t;

/* Init/deinit helpers (optional; measurements auto-init). */
HAL_StatusTypeDef BME280_Init(I2C_HandleTypeDef *hi2c);
HAL_StatusTypeDef BME280_Deinit(void);

/* Measurement functions (auto init/deinit). */
HAL_StatusTypeDef RH(float *humidity);         /* Relative humidity (%) */
HAL_StatusTypeDef T(float *temperature);       /* Temperature (C) */
HAL_StatusTypeDef P(float *pressure);          /* Pressure (hPa) */
HAL_StatusTypeDef BME280(BME280_Data_t *data); /* All readings */

#endif /* PROJECT_DRV_BME280_H_ */
