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

/* USB CLI helper: sink for writing strings (e.g., USB CDC). */
typedef void (*mic_write_fn_t)(const char *s);

/* USB CLI helper: wait until user presses ENTER (return 1), or timeout/disconnect (return 0). */
typedef uint8_t (*mic_wait_enter_fn_t)(uint32_t timeout_ms);

/* Optional helper: start a calibration tone (used by MICCAL()). */
typedef void (*mic_beep_fn_t)(uint16_t freq_hz, uint8_t volume, float time_s);

/* Microphone DSP/capture tunables. */
#ifndef MIC_DECIM_N
#define MIC_DECIM_N      10u     /* 16-bit SPI words: bit-decimation = 16*MIC_DECIM_N (e.g. 128 @ MIC_DECIM_N=8) */
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
#define MIC_FIR_TAPS     2u     /* moving-average smoothing taps on the decimated stream (keep small to preserve HF band) */
#endif

/*
 * dBFS calibration (optional)
 *
 * The driver computes dBFS from the RMS of the band-limited decoded signal where
 * full-scale is |x|=1.0.
 *
 * Defaults keep the raw scale unchanged (and can be overridden at runtime by MIC_CalLoad()):
 *   MIC_CAL_RMS_GAIN      = 1.0f
 *   MIC_CAL_DB_OFFSET_DB  = 0.0f
 *
 * For microphones that specify sensitivity in dBFS @ 94 dB SPL, 1 kHz, you can
 * align the reading by applying an offset (and/or gain).
 *
 * Example (CMM-4030DT-26154-TR datasheet):
 *   Sensitivity S ≈ -27..-25 dBFS @ 94 dB SPL, 1 kHz
 *   SNR (A-weighted) ≈ 65 dBA @ 94 dB SPL, 1 kHz
 *   AOP ≈ 120 dB SPL @ 10% THD, 1 kHz  (roughly corresponds to ~0 dBFS)
 *
 * If you can produce (or otherwise know) an input SPL and your measured MIC()
 * differs from the expected dBFS, adjust MIC_CAL_DB_OFFSET_DB accordingly.
 */
#ifndef MIC_CAL_RMS_GAIN
#define MIC_CAL_RMS_GAIN      1.0f
#endif
#ifndef MIC_CAL_DB_OFFSET_DB
#define MIC_CAL_DB_OFFSET_DB  0.0f
#endif

/*
 * Persistent calibration storage:
 * By default, mic calibration offset can be stored in an RTC backup register so it
 * survives reset and "off/on" while the backup domain is powered (battery).
 */
#ifndef MIC_CAL_PERSIST_BKP_REG
#define MIC_CAL_PERSIST_BKP_REG RTC_BKP_DR2
#endif

/*
 * MICCAL() interactive calibration (USB CLI helper).
 *
 * The firmware reports dBFS from the decoded band-limited PCM stream. To align the reading with a
 * particular microphone part, MICCAL estimates expected dBFS from the microphone sensitivity spec
 * (dBFS @ 94 dB SPL) and the applied SPL, then stores a dB offset.
 *
 * Modes:
 *   - MICCAL()      : auto mode using the on-board buzzer as a crude SPL reference (see *_AUTO_* below)
 *   - MICCAL(x)     : manual mode, x = dB SPL at the microphone (external known source)
 *
 * Workflow (both modes):
 *   1) Press ENTER when quiet  -> measure noise floor
 *   2) Press ENTER when audio  -> measure audio
 * The code subtracts noise power from audio power before computing dBFS.
 */
#ifndef MIC_CAL_SENS_DBFS_AT_94SPL
#define MIC_CAL_SENS_DBFS_AT_94SPL (-26.0f) /* CMM-4030DT-26154-TR typical: -27..-25 dBFS @ 94 dB SPL, 1 kHz */
#endif

#ifndef MIC_CAL_AUTO_REF_SPL_DB
#define MIC_CAL_AUTO_REF_SPL_DB (65.0f) /* buzzer: dB SPL @ MIC_CAL_AUTO_REF_DIST_CM (4kHz square), datasheet */
#endif
#ifndef MIC_CAL_AUTO_REF_DIST_CM
#define MIC_CAL_AUTO_REF_DIST_CM (10.0f) /* buzzer reference distance */
#endif
#ifndef MIC_CAL_AUTO_MIC_DIST_CM
#define MIC_CAL_AUTO_MIC_DIST_CM (3.3f)  /* actual mic<->buzzer distance on PCB */
#endif

#ifndef MIC_CAL_AUTO_TONE_FREQ_HZ
#define MIC_CAL_AUTO_TONE_FREQ_HZ (4000u)
#endif
#ifndef MIC_CAL_AUTO_TONE_VOL
#define MIC_CAL_AUTO_TONE_VOL (50u) /* project clamps volume to 0..50 */
#endif
#ifndef MIC_CAL_AUTO_TONE_SETTLE_MS
#define MIC_CAL_AUTO_TONE_SETTLE_MS (200u)
#endif

#ifndef MIC_CAL_MEASURE_MS
#define MIC_CAL_MEASURE_MS (2000u)
#endif

#ifndef MIC_CAL_WAIT_ENTER_TIMEOUT_MS
#define MIC_CAL_WAIT_ENTER_TIMEOUT_MS (0u) /* 0 = wait forever */
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
 * Runtime calibration control (used by USB CLI MICCAL()).
 * NOTE: MIC_CalSave()/Load() persist only MIC_CAL_DB_OFFSET_DB (centi-dB) in RTC BKP.
 */
void MIC_CalResetToDefaults(void);
void MIC_CalGet(float *out_rms_gain, float *out_db_offset_db);
void MIC_CalSet(float rms_gain, float db_offset_db);
uint8_t MIC_CalLoad(void);
uint8_t MIC_CalSave(void);

/* Interactive calibration used by USB CLI MICCAL(). */
mic_err_t MIC_CalibrateInteractiveAuto(mic_write_fn_t write,
                                      mic_wait_enter_fn_t wait_enter,
                                      mic_beep_fn_t beep);
mic_err_t MIC_CalibrateInteractiveManualSpl(float spl_db,
                                            mic_write_fn_t write,
                                            mic_wait_enter_fn_t wait_enter);

/*
 * Return the last completed 50 ms window result.
 * Returns MIC_ERR_OK on valid data; otherwise out_* is 0 or last known.
 */
mic_err_t MIC_GetLast50ms(float *out_dbfs, float *out_rms);

/* Same as MIC_GetLast50ms(), but also returns a monotonically increasing window sequence number. */
mic_err_t MIC_GetLast50msEx(float *out_dbfs, float *out_rms, uint32_t *out_seq);

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
 *     MIC_WAKEUP_MS (datasheet ~52ms, default uses extra margin) on top, because the mic needs
 *     time before it
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
#define MIC_WAKEUP_MS   70u
#endif

/*
 * Audio band limits (applied to both MIC() and MICFFT()).
 *
 * NOTE: To support MIC_BAND_LP_HZ, the PCM sample-rate after decimation must satisfy
 * Nyquist: fs > 2*MIC_BAND_LP_HZ. If you raise MIC_BAND_LP_HZ, you may need to lower
 * MIC_DECIM_N (higher PCM fs) at the cost of CPU.
 */
#ifndef MIC_BAND_HP_HZ
#define MIC_BAND_HP_HZ   100u
#endif
#ifndef MIC_BAND_LP_HZ
#define MIC_BAND_LP_HZ  8000u
#endif

/* Backward compatible names (no longer used directly by the driver). */
#ifndef MIC_RMS_HP_HZ
#define MIC_RMS_HP_HZ   MIC_BAND_HP_HZ
#endif

/*
 * MICFFT (3-band "FFT-like" analysis) configuration.
 * Used for "color music" effects: computes LF/MF/HF levels as dBFS*100 (int16).
 */
#ifndef MICFFT_WINDOW_MS
#define MICFFT_WINDOW_MS  100u   /* averaging window (ms), must be <= 1000ms */
#endif

/* Backward compatible names (driver uses MIC_BAND_*). */
#ifndef MICFFT_HP_HZ
#define MICFFT_HP_HZ      MIC_BAND_HP_HZ
#endif
#ifndef MICFFT_LF_MAX_HZ
#define MICFFT_LF_MAX_HZ  400u   /* LF: MICFFT_HP_HZ..MICFFT_LF_MAX_HZ */
#endif
#ifndef MICFFT_MF_MAX_HZ
#define MICFFT_MF_MAX_HZ  2000u  /* MF: MICFFT_LF_MAX_HZ..MICFFT_MF_MAX_HZ */
#endif
#ifndef MICFFT_HF_MAX_HZ
#define MICFFT_HF_MAX_HZ  MIC_BAND_LP_HZ
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

/* USB CLI helper: microphone diagnostics (debug). */
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
