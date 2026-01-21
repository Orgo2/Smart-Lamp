#pragma once
#include "stm32u0xx_hal.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * mic.h - PDM microphone capture over SPI1 + DMA.
 * Pins (CubeMX): PA5=SPI1_SCK, PA6=SPI1_MISO (PDM data).
 */

typedef enum
{
    MIC_ERR_OK = 0,

    /* Configuration / init errors. */
    MIC_ERR_NOT_INIT = -1,
    MIC_ERR_SPI_NOT_READY = -2,

    /* Runtime capture errors. */
    MIC_ERR_START_DMA = -3,
    MIC_ERR_TIMEOUT = -4,
    MIC_ERR_SPI_ERROR = -5,
    MIC_ERR_DMA_NO_WRITE = -6,

    /* Data quality / wiring errors. */
    MIC_ERR_DATA_STUCK = -7,       /* samples look constant (DATA stuck at 0/1) */
    MIC_ERR_SIGNAL_SATURATED = -8, /* looks like saturation (RMS or peak ~ 1.0) */
    /* State: capture running but no data yet */
    MIC_ERR_NO_DATA_YET = -9,
} mic_err_t;

/* Call once after MX_SPI1_Init() (e.g., in main USER CODE BEGIN 2). */
void MIC_Init(void);

/* Call periodically (e.g., in the main loop). Updates the 50 ms RMS window. */
void MIC_Task(void);

/* Start capture (if already running, returns OK). */
mic_err_t MIC_Start(void);

/* Stop capture (aborts DMA). */
void MIC_Stop(void);

/*
 * Return the last completed 50 ms window result.
 * Returns MIC_ERR_OK on valid data; otherwise out_* is 0 or last known.
 */
mic_err_t MIC_GetLast50ms(float *out_dbfs, float *out_rms);

/* Enable/disable verbose printf debug (UART). Default: off. */
void MIC_SetDebug(uint8_t enable);

/*
 * MIC_POWERSAVE (compile-time): 0=continuous, >0=one-shot capture length in ms (min 10 ms).
 */
#ifndef MIC_POWERSAVE
#define MIC_POWERSAVE   0u
#endif

/*
 * Microphone wake-up time after clock start (ignore samples during this time).
 */
#ifndef MIC_WAKEUP_MS
#define MIC_WAKEUP_MS   52u
#endif

/* Legacy API kept for CLI compatibility. */

/* One-shot measurement and UART debug output (printf). */
float MIC_ReadDbFS_Debug(void);

/* One-shot measurement without extra text (currently calls Debug). */
float MIC_ReadDbFS(void);

/* Last computed level (dBFS). */
float MIC_LastDbFS(void);

/* Last computed RMS "energy" from decoded PCM (0..1). */
float MIC_LastRms(void);

/* Debug: last error message (or NULL). */
const char* MIC_LastErrorMsg(void);

/* Debug: get last DMA buffer for CLI inspection */
const uint16_t* MIC_DebugLastDmaBuf(uint32_t *out_words);

#ifdef __cplusplus
}
#endif
