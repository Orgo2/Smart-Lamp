#pragma once
#include "stm32u0xx_hal.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * PDM mikrofon na SPI1 (CubeMX):
 *  - PA5 = SPI1_SCK  (hodiny pre mikrofón)
 *  - PA6 = SPI1_MISO (DATA z mikrofónu)
 *
 * Tento modul používa iba HAL (žiadne priame registre) a číta dáta cez SPI1+DMA.
 *
 * =============================================================
 * "Plnohodnotný" režim pre farebnú hudbu (VU meter / beat detect):
 * =============================================================
 *  - spracovanie v 50 ms oknách (20 Hz update)
 *  - výstup: RMS úroveň v dBFS (alebo dB re:FS)
 *  - interná decimácia PDM->PCM na cca 4..8 kSPS (konfigurovateľné)
 */

typedef enum
{
    MIC_ERR_OK = 0,

    /* Konfig / init chyby */
    MIC_ERR_NOT_INIT = -1,
    MIC_ERR_SPI_NOT_READY = -2,

    /* Runtime chyby počas snímania */
    MIC_ERR_START_DMA = -3,
    MIC_ERR_TIMEOUT = -4,
    MIC_ERR_SPI_ERROR = -5,
    MIC_ERR_DMA_NO_WRITE = -6,

    /* Kvalita dát / zapojenie */
    MIC_ERR_DATA_STUCK = -7,       /* všetky vzorky vyzerajú rovnaké (DATA stuck na 0/1) */
    MIC_ERR_SIGNAL_SATURATED = -8, /* vyzerá to ako saturácia (RMS alebo peak ~ 1.0) */
    /* State: capture running but no data yet */
    MIC_ERR_NO_DATA_YET = -9,
} mic_err_t;

/* Zavolaj raz po MX_SPI1_Init() (napr. v main USER CODE BEGIN 2). */
void MIC_Init(void);

/* Volaj často (napr. v main while loop). Spracuje 50ms okná na pozadí. */
void MIC_Task(void);

/* Spustí kontinuálne snímanie (ak už beží, nič neurobí). */
mic_err_t MIC_Start(void);

/* Zastaví snímanie (ukončí DMA). */
void MIC_Stop(void);

/*
 * Vráti posledný hotový výsledok z 50 ms okna.
 *
 * Návrat:
 *  - MIC_ERR_OK: hodnoty sú platné
 *  - inak: chyba (pozri mic_err_t), a v out_* je posledné známe alebo 0
 */
mic_err_t MIC_GetLast50ms(float *out_dbfs, float *out_rms);

/* Zapne/vypne detailný printf debug (UART). Default: vypnuté. */
void MIC_SetDebug(uint8_t enable);

/*
 * ===================== MIC PowerSave (TEST / compile-time) =====================
 *
 * Význam hodnoty:
 *  - 0      -> MIC beží stále (continuous), SPI1 SCK beží stále počas snímania.
 *  - 1..10  -> minimálne 10 ms (krátke hodnoty sa zneplatnia, aby MIC stihol odmerať aspoň niečo)
 *  - >10    -> čas behu v ms
 *
 * Štartovacia hodnota podľa zadania: 100 ms.
 */
#ifndef MIC_POWERSAVE
#define MIC_POWERSAVE   100u
#endif

/*
 * CMM-4030DT-26154: ~52 ms wake-up po spustení clocku.
 * Počas tejto doby môžu byť dáta stuck/nezmyselné, preto ich driver ignoruje.
 */
#ifndef MIC_WAKEUP_MS
#define MIC_WAKEUP_MS   52u
#endif

/* ================= Legacy API (ponechané kvôli existujúcemu CLI) ================ */

/* Jednorazové meranie a výpis debug informácií na UART (printf). */
float MIC_ReadDbFS_Debug(void);

/* Jednorazové meranie bez extra textov (aktuálne volá Debug). */
float MIC_ReadDbFS(void);

/* Posledná vypočítaná úroveň signálu (dBFS). */
float MIC_LastDbFS(void);

/* Posledná vypočítaná "energia" (RMS) z dekódovaného PCM (0..1). */
float MIC_LastRms(void);

/* Debug: last error message (or NULL). */
const char* MIC_LastErrorMsg(void);

/* Debug: get last DMA buffer for CLI inspection */
const uint16_t* MIC_DebugLastDmaBuf(uint32_t *out_words);

#ifdef __cplusplus
}
#endif
