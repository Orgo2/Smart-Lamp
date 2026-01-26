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

/* Microphone DSP/capture tunables. */
#ifndef MIC_DECIM_N
#define MIC_DECIM_N      8u     /* decimation factor (quality/CPU trade-off) */
#endif
#ifndef MIC_WINDOW_MS
#define MIC_WINDOW_MS    50u    /* RMS/dBFS window length */
#endif
#ifndef MIC_DMA_WORDS
#define MIC_DMA_WORDS    512u   /* DMA block size (words) */
#endif
#ifndef MIC_TIMEOUT_MS
#define MIC_TIMEOUT_MS   200u   /* DMA timeout waiting for RxCplt */
#endif
#ifndef MIC_FIR_TAPS
#define MIC_FIR_TAPS     8u     /* moving-average smoothing taps */
#endif

/* SPI1 and GPIO pins are configured by CubeMX (.ioc). */

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
 * MIC_POWERSAVE (compile-time):
 *   0   = keep microphone clock running continuously (debug / lowest latency).
 *   >0  = power-save mode: do periodic measurements and stop the MIC clock afterwards.
 *
 * Semantics:
 *   - MIC_POWERSAVE is the requested "useful" capture time in ms.
 *   - If the MIC clock was OFF before the measurement, the driver automatically adds
 *     MIC_WAKEUP_MS (datasheet: 52ms) on top, because the mic needs time before it
 *     starts outputting valid PDM data.
 *   - If MIC_POWERSAVE is too small to produce at least one RMS window, it is clamped
 *     to MIC_WINDOW_MS internally.
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

/*
 * MICFFT (3-band "FFT-like" analysis) configuration.
 * Used for "color music" effects: computes LF/MF/HF levels as dBFS*100 (int16).
 */
#ifndef MICFFT_WINDOW_MS
#define MICFFT_WINDOW_MS  500u   /* averaging window (ms), must be <= 1000ms */
#endif
#ifndef MICFFT_HP_HZ
#define MICFFT_HP_HZ      100u   /* remove below this (high-pass via subtraction) */
#endif
#ifndef MICFFT_LF_MAX_HZ
#define MICFFT_LF_MAX_HZ  400u   /* LF: MICFFT_HP_HZ..MICFFT_LF_MAX_HZ */
#endif
#ifndef MICFFT_MF_MAX_HZ
#define MICFFT_MF_MAX_HZ  1600u  /* MF: MICFFT_LF_MAX_HZ..MICFFT_MF_MAX_HZ */
#endif
#ifndef MICFFT_HF_MAX_HZ
#define MICFFT_HF_MAX_HZ  4000u  /* HF: MICFFT_MF_MAX_HZ..MICFFT_HF_MAX_HZ */
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

/* USB CLI helper: find working SPI CPOL/CPHA for the PDM mic (debug). */
typedef void (*mic_write_fn_t)(const char *s);
void MIC_FindMic(mic_write_fn_t write);
void MIC_WriteDiag(mic_write_fn_t write);

/* Error name helper (for debug prints). */
const char* MIC_ErrName(mic_err_t e);

/*
 * Blocking helper used by MiniPascal/CLI: wait for a valid 50ms window.
 * Returns MIC_ERR_OK and writes dBFS*100 (int16) on success.
 */
mic_err_t MIC_ReadDbfsX100_Blocking(uint32_t timeout_ms, int16_t *out_dbfs_x100);

/*
 * MICFFT bins (LF/MF/HF) as dBFS*100 (int16).
 * NOTE: internally this is a lightweight 3-band filterbank, not a full FFT.
 */
mic_err_t MIC_FFT_GetLastBinsDbX100(int16_t *out_lf_db_x100,
                                   int16_t *out_mf_db_x100,
                                   int16_t *out_hf_db_x100);
mic_err_t MIC_FFT_WaitBinsDbX100(uint32_t timeout_ms,
                                int16_t *out_lf_db_x100,
                                int16_t *out_mf_db_x100,
                                int16_t *out_hf_db_x100);

#ifdef __cplusplus
}
#endif
