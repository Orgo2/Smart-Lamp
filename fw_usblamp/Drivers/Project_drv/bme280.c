/*
 * bme280.c
 */

#include "bme280.h"
#include "main.h"
#include <string.h>

// External I2C handle (defined in main.c)
extern I2C_HandleTypeDef hi2c1;

// BME280 Registers
#define BME280_REG_ID           0xD0
#define BME280_REG_RESET        0xE0
#define BME280_REG_CTRL_HUM     0xF2
#define BME280_REG_CTRL_MEAS    0xF4
#define BME280_REG_CONFIG       0xF5
#define BME280_REG_PRESS_MSB    0xF7
#define BME280_REG_CALIB00      0x88
#define BME280_REG_CALIB26      0xE1

#define BME280_CHIP_ID          0x60
#define BME280_SOFT_RESET       0xB6
#define BME280_SLEEP_MODE       0x00
#define BME280_NORMAL_MODE      0x03
#define BME280_OVERSAMPLING_16X 0x05
#define BME280_FILTER_16        0x04
#define BME280_STANDBY_0_5_MS   0x00

#define BME280_TIMEOUT_MS       5000
#define BME280_I2C_TIMEOUT_MS   500   // I2C operation timeout

typedef struct {
    uint16_t dig_T1;
    int16_t  dig_T2;
    int16_t  dig_T3;
    uint16_t dig_P1;
    int16_t  dig_P2;
    int16_t  dig_P3;
    int16_t  dig_P4;
    int16_t  dig_P5;
    int16_t  dig_P6;
    int16_t  dig_P7;
    int16_t  dig_P8;
    int16_t  dig_P9;
    uint8_t  dig_H1;
    int16_t  dig_H2;
    uint8_t  dig_H3;
    int16_t  dig_H4;
    int16_t  dig_H5;
    int8_t   dig_H6;
} BME280_CalibData_t;

static I2C_HandleTypeDef *bme280_i2c = NULL;
static uint8_t bme280_addr = BME280_I2C_ADDR << 1;
static BME280_CalibData_t calib_data;
static int32_t t_fine;
static uint32_t init_time = 0;

static HAL_StatusTypeDef BME280_WriteReg(uint8_t reg, uint8_t value);
static HAL_StatusTypeDef BME280_ReadReg(uint8_t reg, uint8_t *value);
static HAL_StatusTypeDef BME280_ReadRegs(uint8_t reg, uint8_t *buffer, uint16_t len);
static HAL_StatusTypeDef BME280_ReadCalibrationData(void);
static int32_t BME280_CompensateT(int32_t adc_T);
static uint32_t BME280_CompensateP(int32_t adc_P);
static uint32_t BME280_CompensateH(int32_t adc_H);
static HAL_StatusTypeDef BME280_ReadSensorData(BME280_Data_t *data);

HAL_StatusTypeDef BME280_Init(I2C_HandleTypeDef *hi2c)
{
    if (hi2c == NULL) {
        return HAL_ERROR;
    }
    
    bme280_i2c = hi2c;
    init_time = HAL_GetTick();
    
    uint8_t chip_id = 0;
    if (BME280_ReadReg(BME280_REG_ID, &chip_id) != HAL_OK) {
        bme280_i2c = NULL;
        return HAL_ERROR;
    }
    
    if (chip_id != BME280_CHIP_ID) {
        bme280_i2c = NULL;
        return HAL_ERROR;
    }
    
    BME280_WriteReg(BME280_REG_RESET, BME280_SOFT_RESET);
    HAL_Delay(10);
    
    if (BME280_ReadCalibrationData() != HAL_OK) {
        bme280_i2c = NULL;
        return HAL_ERROR;
    }
    
    // Max accuracy: oversampling x16, filter x16
    BME280_WriteReg(BME280_REG_CTRL_HUM, BME280_OVERSAMPLING_16X);
    
    uint8_t config = (BME280_STANDBY_0_5_MS << 5) | (BME280_FILTER_16 << 2);
    BME280_WriteReg(BME280_REG_CONFIG, config);
    
    uint8_t ctrl_meas = (BME280_OVERSAMPLING_16X << 5) | 
                        (BME280_OVERSAMPLING_16X << 2) | 
                        BME280_NORMAL_MODE;
    BME280_WriteReg(BME280_REG_CTRL_MEAS, ctrl_meas);
    
    HAL_Delay(100);
    
    return HAL_OK;
}

HAL_StatusTypeDef BME280_Deinit(void)
{
    if (bme280_i2c == NULL) {
        return HAL_ERROR;
    }
    
    uint8_t ctrl_meas = (BME280_OVERSAMPLING_16X << 5) | 
                        (BME280_OVERSAMPLING_16X << 2) | 
                        BME280_SLEEP_MODE;
    BME280_WriteReg(BME280_REG_CTRL_MEAS, ctrl_meas);
    
    bme280_i2c = NULL;
    return HAL_OK;
}

HAL_StatusTypeDef RH(float *humidity)
{
    if (BME280_Init(&hi2c1) != HAL_OK) {
        return HAL_ERROR;
    }
    HAL_Delay(100); // Stabilizácia senzora
    
    BME280_Data_t data;
    HAL_StatusTypeDef status = BME280_ReadSensorData(&data);
    BME280_Deinit();
    
    if (status == HAL_OK) {
        *humidity = data.humidity;
    }
    return status;
}

HAL_StatusTypeDef T(float *temperature)
{
    if (BME280_Init(&hi2c1) != HAL_OK) {
        return HAL_ERROR;
    }
    HAL_Delay(100); // Stabilizácia senzora
    
    BME280_Data_t data;
    HAL_StatusTypeDef status = BME280_ReadSensorData(&data);
    BME280_Deinit();
    
    if (status == HAL_OK) {
        *temperature = data.temperature;
    }
    return status;
}

HAL_StatusTypeDef P(float *pressure)
{
    if (BME280_Init(&hi2c1) != HAL_OK) {
        return HAL_ERROR;
    }
    HAL_Delay(100); // Stabilizácia senzora
    
    BME280_Data_t data;
    HAL_StatusTypeDef status = BME280_ReadSensorData(&data);
    BME280_Deinit();
    
    if (status == HAL_OK) {
        *pressure = data.pressure;
    }
    return status;
}

HAL_StatusTypeDef BME280(BME280_Data_t *data)
{
    if (BME280_Init(&hi2c1) != HAL_OK) {
        return HAL_ERROR;
    }
    HAL_Delay(100); // Stabilizácia senzora
    
    HAL_StatusTypeDef status = BME280_ReadSensorData(data);
    BME280_Deinit();
    
    return status;
}

static HAL_StatusTypeDef BME280_WriteReg(uint8_t reg, uint8_t value)
{
    if ((HAL_GetTick() - init_time) > BME280_TIMEOUT_MS) {
        bme280_i2c = NULL;
        return HAL_TIMEOUT;
    }
    return HAL_I2C_Mem_Write(bme280_i2c, bme280_addr, reg, I2C_MEMADD_SIZE_8BIT, &value, 1, BME280_I2C_TIMEOUT_MS);
}

static HAL_StatusTypeDef BME280_ReadReg(uint8_t reg, uint8_t *value)
{
    if ((HAL_GetTick() - init_time) > BME280_TIMEOUT_MS) {
        bme280_i2c = NULL;
        return HAL_TIMEOUT;
    }
    return HAL_I2C_Mem_Read(bme280_i2c, bme280_addr, reg, I2C_MEMADD_SIZE_8BIT, value, 1, BME280_I2C_TIMEOUT_MS);
}

static HAL_StatusTypeDef BME280_ReadRegs(uint8_t reg, uint8_t *buffer, uint16_t len)
{
    if ((HAL_GetTick() - init_time) > BME280_TIMEOUT_MS) {
        bme280_i2c = NULL;
        return HAL_TIMEOUT;
    }
    return HAL_I2C_Mem_Read(bme280_i2c, bme280_addr, reg, I2C_MEMADD_SIZE_8BIT, buffer, len, BME280_I2C_TIMEOUT_MS);
}

static HAL_StatusTypeDef BME280_ReadSensorData(BME280_Data_t *data)
{
    if (bme280_i2c == NULL || data == NULL) {
        return HAL_ERROR;
    }
    
    uint8_t sensor_data[8];
    if (BME280_ReadRegs(BME280_REG_PRESS_MSB, sensor_data, 8) != HAL_OK) {
        return HAL_ERROR;
    }
    
    int32_t adc_P = (sensor_data[0] << 12) | (sensor_data[1] << 4) | (sensor_data[2] >> 4);
    int32_t adc_T = (sensor_data[3] << 12) | (sensor_data[4] << 4) | (sensor_data[5] >> 4);
    int32_t adc_H = (sensor_data[6] << 8) | sensor_data[7];
    
    int32_t temp = BME280_CompensateT(adc_T);
    data->temperature = temp / 100.0f;
    
    uint32_t press = BME280_CompensateP(adc_P);
    data->pressure = press / 256.0f / 100.0f;
    
    uint32_t hum = BME280_CompensateH(adc_H);
    data->humidity = hum / 1024.0f;
    
    init_time = HAL_GetTick();  // Reset timeout on successful read
    
    return HAL_OK;
}

static HAL_StatusTypeDef BME280_ReadCalibrationData(void)
{
    uint8_t calib[26];
    uint8_t calib_h[7];
    
    if (BME280_ReadRegs(BME280_REG_CALIB00, calib, 26) != HAL_OK) {
        return HAL_ERROR;
    }
    
    if (BME280_ReadRegs(BME280_REG_CALIB26, calib_h, 7) != HAL_OK) {
        return HAL_ERROR;
    }
    
    calib_data.dig_T1 = (calib[1] << 8) | calib[0];
    calib_data.dig_T2 = (calib[3] << 8) | calib[2];
    calib_data.dig_T3 = (calib[5] << 8) | calib[4];
    
    calib_data.dig_P1 = (calib[7] << 8) | calib[6];
    calib_data.dig_P2 = (calib[9] << 8) | calib[8];
    calib_data.dig_P3 = (calib[11] << 8) | calib[10];
    calib_data.dig_P4 = (calib[13] << 8) | calib[12];
    calib_data.dig_P5 = (calib[15] << 8) | calib[14];
    calib_data.dig_P6 = (calib[17] << 8) | calib[16];
    calib_data.dig_P7 = (calib[19] << 8) | calib[18];
    calib_data.dig_P8 = (calib[21] << 8) | calib[20];
    calib_data.dig_P9 = (calib[23] << 8) | calib[22];
    
    calib_data.dig_H1 = calib[25];
    calib_data.dig_H2 = (calib_h[1] << 8) | calib_h[0];
    calib_data.dig_H3 = calib_h[2];
    calib_data.dig_H4 = (calib_h[3] << 4) | (calib_h[4] & 0x0F);
    calib_data.dig_H5 = (calib_h[5] << 4) | (calib_h[4] >> 4);
    calib_data.dig_H6 = calib_h[6];
    
    return HAL_OK;
}

static int32_t BME280_CompensateT(int32_t adc_T)
{
    int32_t var1, var2, T;
    
    var1 = ((((adc_T >> 3) - ((int32_t)calib_data.dig_T1 << 1))) * ((int32_t)calib_data.dig_T2)) >> 11;
    var2 = (((((adc_T >> 4) - ((int32_t)calib_data.dig_T1)) * ((adc_T >> 4) - ((int32_t)calib_data.dig_T1))) >> 12) * ((int32_t)calib_data.dig_T3)) >> 14;
    t_fine = var1 + var2;
    T = (t_fine * 5 + 128) >> 8;
    
    return T;
}

static uint32_t BME280_CompensateP(int32_t adc_P)
{
    int64_t var1, var2, p;
    
    var1 = ((int64_t)t_fine) - 128000;
    var2 = var1 * var1 * (int64_t)calib_data.dig_P6;
    var2 = var2 + ((var1 * (int64_t)calib_data.dig_P5) << 17);
    var2 = var2 + (((int64_t)calib_data.dig_P4) << 35);
    var1 = ((var1 * var1 * (int64_t)calib_data.dig_P3) >> 8) + ((var1 * (int64_t)calib_data.dig_P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1)) * ((int64_t)calib_data.dig_P1) >> 33;
    
    if (var1 == 0) {
        return 0;
    }
    
    p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)calib_data.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)calib_data.dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)calib_data.dig_P7) << 4);
    
    return (uint32_t)p;
}

static uint32_t BME280_CompensateH(int32_t adc_H)
{
    int32_t v_x1_u32r;
    
    v_x1_u32r = (t_fine - ((int32_t)76800));
    v_x1_u32r = (((((adc_H << 14) - (((int32_t)calib_data.dig_H4) << 20) - (((int32_t)calib_data.dig_H5) * v_x1_u32r)) +
                ((int32_t)16384)) >> 15) * (((((((v_x1_u32r * ((int32_t)calib_data.dig_H6)) >> 10) *
                (((v_x1_u32r * ((int32_t)calib_data.dig_H3)) >> 11) + ((int32_t)32768))) >> 10) + ((int32_t)2097152)) *
                ((int32_t)calib_data.dig_H2) + 8192) >> 14));
    v_x1_u32r = (v_x1_u32r - (((((v_x1_u32r >> 15) * (v_x1_u32r >> 15)) >> 7) * ((int32_t)calib_data.dig_H1)) >> 4));
    v_x1_u32r = (v_x1_u32r < 0 ? 0 : v_x1_u32r);
    v_x1_u32r = (v_x1_u32r > 419430400 ? 419430400 : v_x1_u32r);
    
    return (uint32_t)(v_x1_u32r >> 12);
}