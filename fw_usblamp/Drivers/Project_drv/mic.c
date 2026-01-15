/*
 * mic.c — PDM mikrofón cez SPI1 + DMA (HAL only)
 *
 * =============================================================
 * 0) Čo je cieľ tohoto súboru?
 * =============================================================
 * Chceme si "na stole" overiť, že:
 *  - SPI1 generuje hodiny (CLOCK) pre PDM mikrofón
 *  - z mikrofónu prichádzajú dáta (DATA) a DMA ich zapisuje do RAM
 *  - vieme z dát spraviť jednoduchý odhad "sily" signálu (RMS / dBFS)
 *
 * Tento kód NIE JE plnohodnotný audio driver.
 * Je to jednoduchý debug/test pre začiatočníka.
 *
 * =============================================================
 * 1) Ako PDM mikrofón funguje (ľudsky):
 * =============================================================
 * PCM (bežné audio): 16-bit číslo = amplitúda v čase.
 * PDM (mikrofón): 1-bit stream (0/1) veľmi rýchlo.
 *  - keď je viac "1" v okne, signál je viac kladný
 *  - keď je viac "0" v okne, signál je viac záporný
 *
 * Preto robíme popcount (počítame jednotky v 16 bitoch).
 *
 * =============================================================
 * 2) Hardware (podľa CubeMX / zadania):
 * =============================================================
 *  PA5 = SPI1_SCK  (CLOCK)  -> hodiny pre mikrofón
 *  PA6 = SPI1_MISO (DATA)   -> PDM data z mikrofónu
 *
 * Pozor: CLOCK na PA5 NEBEŽÍ stále.
 * Beží len počas SPI prenosu (keď je SPI v stave BUSY).
 *
 * =============================================================
 * 3) Ako debugovať, keď to nefunguje:
 * =============================================================
 * A) HAL_SPI_Receive_DMA() zlyhá
 *    -> problém v SPI/DMA konfigurácii (CubeMX), alebo periféria je BUSY z iného dôvodu
 *
 * B) HAL_SPI_Receive_DMA() je OK, ale timeout
 *    -> DMA interrupt nechodí (NVIC), alebo SPI sa nerozbehlo
 *
 * C) DMA COMPLETE príde, ale buffer ostane 0xAAAA
 *    -> DMA nič nezapísalo do pamäte (zlé DMA nastavenie / alignment / size / DataSize)
 *
 * D) Buffer sa zmení, ale všetky hodnoty sú rovnaké (napr. FFFF alebo 0000)
 *    -> DATA je stuck (zle zapojenie, mikrofón nevysiela, pullup/pulldown)
 */

#include "main.h"
#include "mic.h"
#include "stm32u0xx_hal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

/* SPI1 handle je vytvorený CubeMX v main.c */
extern SPI_HandleTypeDef hspi1;

/* =====================================================================================
 * Konfigurácia pre "farebnú hudbu" (VU meter)
 * =====================================================================================
 *
 * Chceme výsledok každých 50 ms (20 Hz).
 *
 * PDM->PCM decimácia (jednoduchá):
 *  - 1x 16-bit word (16 PDM bitov) -> 1 float sample
 *  - To znamená, že PCM sample rate ~= SPI_bit_rate / 16
 *
 * Typický PDM mikrofón chce 1..3 MHz clock.
 * V tvojom projekte je SPI clock okolo ~750 kHz (podľa pôvodného komentára),
 * takže PCM rate je cca 750k/16 = 46.9 kSPS. To je viac než potrebujeme.
 *
 * Preto urobíme ďalšiu decimáciu: zoberieme len každý N-tý sample.
 * Nastavené tak, aby výsledný rate bol cca 6.25 kSPS (v strede 4..8kSPS).
 */
#ifndef MIC_DECIM_N
#define MIC_DECIM_N  8u
#endif

/* Koľko ms tvorí jedno okno pre RMS (zadanie: 50 ms) */
#define MIC_WINDOW_MS         50u

/* Počet PCM sample v jednom okne pri 6.25 kSPS: 0.05s * 6250 = 312.5 -> 313 */
#define MIC_WINDOW_SAMPLES    313u

/*
 * SPI DMA blok:
 *  - Nechceme robiť obrovský continuous buffer (komplikovanejšie v CubeMX).
 *  - Urobíme opakované krátke DMA prenosy (ping-pong v softvéri):
 *      spust DMA -> počkaj na complete -> spracuj -> spust ďalší
 *
 * 2048 words pri 16-bit a SPI ~750kHz trvá ~43.7 ms / MIC_DECIM_N? (pdm)
 * Realita: doba závisí od skutočného SPI clock.
 *
 * Zvolíme menší blok, aby sme mali častejšie spracovanie.
 */
#define MIC_DMA_WORDS         512u
#define MIC_TIMEOUT_MS        200u

/* DMA RX buffer (jeden blok) */
static uint16_t s_rx_buf[MIC_DMA_WORDS];

/* riadenie DMA */
static volatile uint8_t s_spi_done;
static volatile uint8_t s_spi_err;
static uint32_t s_dma_t0_ms;
static uint32_t s_capture_t0_ms;

/* driver state */
static uint8_t s_inited;
static uint8_t s_running;
static uint8_t s_debug;
static mic_err_t s_last_err;
static const char *s_last_err_msg;

/* Výsledok posledného okna */
static float s_last_dbfs = -1.0f;
static float s_last_rms  = 0.0f;

/* Akumulátor pre 50 ms okno */
static uint32_t s_win_count;      /* koľko PCM sample sme nazbierali do okna */
static double   s_win_sum_sq;     /* suma s^2 pre RMS */
static double   s_win_peak;       /* peak (voliteľné do debug) */
static uint32_t s_decim_phase;    /* pre "každý N-tý sample" */

/* ===================== PowerSave control (compile-time) =====================
 * MIC_POWERSAVE je nastavené v mic.h a platí až po kompilácii.
 */
static inline uint32_t mic_get_target_ms(void)
{
    /* 0 = continuous */
    if (MIC_POWERSAVE == 0u) return 0u;
    /* 1..10 = minimum 10ms */
    uint32_t target = (MIC_POWERSAVE <= 10u) ? 10u : (uint32_t)MIC_POWERSAVE;

    /* Ensure at least wake-up + one 50ms window worth of time. */
    uint32_t min_target = (uint32_t)MIC_WAKEUP_MS + (uint32_t)MIC_WINDOW_MS;
    if (target < min_target) target = min_target;
    return target;
}

/* last DMA snapshot for CLI dump */
static uint8_t  s_have_last_dma;
static uint32_t s_last_dma_words;

const uint16_t* MIC_DebugLastDmaBuf(uint32_t *out_words)
{
    if (out_words) *out_words = s_last_dma_words;
    return s_have_last_dma ? s_rx_buf : NULL;
}

/* ===================== Interval (one-shot) RMS mode support =====================
 *
 * Keď MIC_PowerSaveMs != 0, chceme:
 *  - spustiť capture
 *  - zbierať približne N ms
 *  - spočítať RMS z nazbieraných decimovaných sample
 *  - uložiť výsledok do s_last_rms / s_last_dbfs (1 premenná)
 *  - zastaviť SPI/DMA (šetrenie energie)
 */
static uint8_t  s_interval_active;
static uint32_t s_interval_t0_ms;

/* ====== pomocné makrá ====== */
#define MIC_DBG(...) do { if (s_debug) printf(__VA_ARGS__); } while (0)

/* ====== pomocné funkcie pre error handling a dBFS ====== */
static float safe_dbfs_from_rms(float rms)
{
    /* ochrana proti log(0) */
    if (rms < 1e-6f) return -120.0f;
    return 20.0f * log10f(rms);
}

static void set_error(mic_err_t e, const char *msg)
{
    s_last_err = e;
    s_last_err_msg = msg;
    if (msg && s_debug)
        printf("[MIC] %s\r\n", msg);
}

/* =====================================================================================
 * "Plnohodnotnejšia" PDM->PCM decimácia (bez ST PDM library)
 * =====================================================================================
 *
 * Zadanie chce zachovať PDM->PCM funkcionalitu s decimáciou, aby bol výstup "audio".
 * V tomto projekte nepoužívame priamo ST PDM_Filter knižnicu (nie je v repozitári),
 * takže implementujeme jednoduchý, ale reálne použiteľný reťazec:
 *
 *  1) CIC 1. rádu (integrator + comb) v časovej doméne PDM
 *  2) decimácia (MIC_DECIM_N)
 *  3) krátky FIR (moving average) - vyhladenie / low-pass
 *
 * Dôležité:
 *  - PCM nevystavujeme ako pole na vonkajší svet.
 *  - Držíme len RMS akumulátor (sum_sq, count) a výsledok (rms, dbfs) v 1 premennej.
 */

/* CIC integrátor / comb stav (1 kanál) */
static int32_t s_cic_integrator;
static int32_t s_cic_comb_prev;

/* FIR moving average (jednoduchý low-pass) */
#define MIC_FIR_TAPS  8u
static int32_t s_fir_hist[MIC_FIR_TAPS];
static uint32_t s_fir_pos;

static void pdm2pcm_reset(void)
{
    s_cic_integrator = 0;
    s_cic_comb_prev  = 0;
    memset(s_fir_hist, 0, sizeof(s_fir_hist));
    s_fir_pos = 0u;
}

static inline uint8_t popcount16_inline(uint16_t x)
{
    uint8_t c = 0;
    while (x)
    {
        x &= (uint16_t)(x - 1u);
        c++;
    }
    return c;
}

/*
 * 16-bit word je 16 PDM bitov.
 * Pre CIC integrátor chceme súčet +1/-1 za tieto bity:
 *  ones - zeros = 2*ones - 16  => rozsah [-16..+16]
 */
static inline int32_t pdm_word_to_cic_input(uint16_t w)
{
    int32_t ones = (int32_t)popcount16_inline(w);
    return (2 * ones) - 16;
}

static inline float pdm2pcm_step(int32_t cic_in)
{
    /* CIC integrátor */
    s_cic_integrator += cic_in;

    /* CIC comb */
    int32_t comb = s_cic_integrator - s_cic_comb_prev;
    s_cic_comb_prev = s_cic_integrator;

    /* FIR moving average */
    s_fir_hist[s_fir_pos] = comb;
    s_fir_pos = (s_fir_pos + 1u) % MIC_FIR_TAPS;

    int64_t acc = 0;
    for (uint32_t i = 0; i < MIC_FIR_TAPS; i++)
        acc += (int64_t)s_fir_hist[i];

    /*
     * Normalizácia:
     *  - tento faktor nie je fyzikálne kalibrovaný, ale drží typický výstup v rozumnom rozsahu.
     *  - keď sa dostane mimo, clipneme.
     */
    float y = (float)acc / (float)(MIC_FIR_TAPS * 256);
    if (y > 1.0f) y = 1.0f;
    if (y < -1.0f) y = -1.0f;
    return y;
}

/* ====== HAL callbacky ====== */
void HAL_SPI_RxCpltCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi == &hspi1)
        s_spi_done = 1u;
}

void HAL_SPI_ErrorCallback(SPI_HandleTypeDef *hspi)
{
    if (hspi == &hspi1)
        s_spi_err = 1u;
}

/* ====== vnútorne: štart jedného DMA bloku ====== */
static mic_err_t start_dma_block(void)
{
    /* Naplň patternom aby sme vedeli zistiť, či DMA písalo */
    memset(s_rx_buf, 0xAA, sizeof(s_rx_buf));

    s_have_last_dma = 0u;
    s_last_dma_words = MIC_DMA_WORDS;

    s_spi_done = 0u;
    s_spi_err  = 0u;

    /* základné sanity checky */
    if (hspi1.Instance != SPI1)
    {
        set_error(MIC_ERR_NOT_INIT, "ERROR: SPI1 not initialized (hspi1.Instance != SPI1)");
        return MIC_ERR_NOT_INIT;
    }

    if (HAL_SPI_GetState(&hspi1) != HAL_SPI_STATE_READY)
    {
        set_error(MIC_ERR_SPI_NOT_READY, "ERROR: SPI not READY (busy from other code?)");
        return MIC_ERR_SPI_NOT_READY;
    }

    /*
     * Štart RX DMA.
     * Size = počet 16-bit elementov (lebo DMA je HALFWORD a SPI je 16-bit v CubeMX).
     */
    HAL_StatusTypeDef st = HAL_SPI_Receive_DMA(&hspi1, (uint8_t*)s_rx_buf, MIC_DMA_WORDS);
    if (st != HAL_OK)
    {
        set_error(MIC_ERR_START_DMA, "ERROR: HAL_SPI_Receive_DMA failed");
        if (s_debug)
            printf("[MIC] HAL st=%d state=%d err=0x%08lX\r\n", (int)st, (int)HAL_SPI_GetState(&hspi1), (unsigned long)hspi1.ErrorCode);
        return MIC_ERR_START_DMA;
    }

    /* Informácia pre debug: BUSY znamená, že SPI generuje CLOCK na PA5 */
    s_dma_t0_ms = HAL_GetTick();
    MIC_DBG("[MIC] DMA started. SPI state=%d (2=BUSY => CLOCK on PA5)\r\n", (int)HAL_SPI_GetState(&hspi1));
    return MIC_ERR_OK;
}

/* ====== vnútorne: spracovanie jedného DMA bloku ====== */
static mic_err_t process_block_and_update_window(void)
{
    /* 1) zisti, či DMA naozaj písalo */
    uint32_t still_aa = 0;
    for (uint32_t i = 0; i < MIC_DMA_WORDS; i++)
        if (s_rx_buf[i] == 0xAAAAu) still_aa++;

    if (still_aa == MIC_DMA_WORDS)
    {
        set_error(MIC_ERR_DMA_NO_WRITE, "ERROR: DMA completed but buffer unchanged (0xAAAA) -> DMA not writing");
        return MIC_ERR_DMA_NO_WRITE;
    }

    /* Ulož snapshot pre CLI dump */
    s_have_last_dma = 1u;
    s_last_dma_words = MIC_DMA_WORDS;

    uint8_t in_warmup = 0u;
    if (MIC_WAKEUP_MS != 0u)
    {
        uint32_t elapsed = HAL_GetTick() - s_capture_t0_ms;
        if (elapsed < (uint32_t)MIC_WAKEUP_MS) in_warmup = 1u;
    }

    /*
     * 1b) Rýchla kontrola kvality dát v tomto DMA bloku.
     *
     * Typický problém, ktorý spôsobí RMS=1.00:
     *  - mikrofón DATA je stuck (napr. stále 1 -> 0xFFFF)
     *  - alebo stále 0 -> 0x0000
     * potom popcount dá stále 16 alebo 0 => sample je +1 alebo -1 => RMS=1.
     *
     * Detekcia: len ak sú 100% všetky dáta zlé (všetko 0x0000 alebo 0xFFFF)
     */
    uint32_t bad_count = 0u;

    for (uint32_t i = 0; i < MIC_DMA_WORDS; i++)
    {
        uint16_t w = s_rx_buf[i];
        /* Počítaj slová, ktoré sú 0x0000 alebo 0xFFFF (typicky stuck) */
        if ((w == 0x0000u) || (w == 0xFFFFu)) bad_count++;
    }

    /* Ak 100% dát je 0x0000 alebo 0xFFFF, DATA je určite stuck */
    if ((!in_warmup) && (bad_count == MIC_DMA_WORDS))
    {
        set_error(MIC_ERR_DATA_STUCK, "ERROR: PDM DATA stuck (all 0x0000 or 0xFFFF)");
        return MIC_ERR_DATA_STUCK;
    }

    /* 2) PDM->PCM (CIC+FIR) + decimácia + RMS accumulator */
    for (uint32_t i = 0; i < MIC_DMA_WORDS; i++)
    {
        /* CIC vstup je suma +1/-1 cez 16 bitov */
        int32_t cic_in = pdm_word_to_cic_input(s_rx_buf[i]);

        /* decimácia: ber len každý MIC_DECIM_N-tý word ako výstup PCM */
        if ((s_decim_phase++ % MIC_DECIM_N) != 0u)
        {
            /* aj keď neberieme výstup, integrátor musí bežať -> voláme step, ale výstup ignorujeme */
            (void)pdm2pcm_step(cic_in);
            continue;
        }

        float s = pdm2pcm_step(cic_in);

        if (in_warmup)
        {
            continue;
        }

        /* Akumuluj RMS */
        double sd = (double)s;
        s_win_sum_sq += sd * sd;

        double a = (sd >= 0.0) ? sd : -sd;
        if (a > s_win_peak) s_win_peak = a;

        s_win_count++;

        /* Keď nazbierame MIC_WINDOW_SAMPLES, vyhodnoť okno */
        if (s_win_count >= MIC_WINDOW_SAMPLES)
        {
            float rms = (float)sqrt(s_win_sum_sq / (double)s_win_count);
            float dbfs = safe_dbfs_from_rms(rms);

            /*
             * Dodatočná ochrana: saturácia/nezmyselné údaje.
             * Ak RMS alebo peak vyjde skoro 1.0, je to pre farebnú hudbu zvyčajne chyba zapojenia.
             */
            if (rms > 0.98f || s_win_peak > 0.98)
            {
                MIC_DBG("[MIC] WARNING: saturation suspected: rms=%.4f peak=%.4f\r\n", (double)rms, (double)s_win_peak);
                set_error(MIC_ERR_SIGNAL_SATURATED, "ERROR: signal saturated (RMS/PEAK ~ 1.0) - likely DATA stuck or wrong clock/polarity");

                /* reset okna, aby sme sa nezasekli na nekonečnom 1.0 */
                s_win_count  = 0u;
                s_win_sum_sq = 0.0;
                s_win_peak   = 0.0;
                return MIC_ERR_SIGNAL_SATURATED;
            }

            s_last_rms  = rms;
            s_last_dbfs = dbfs;
            s_last_err  = MIC_ERR_OK;
            s_last_err_msg = NULL;

            MIC_DBG("[MIC] 50ms window ready: n=%lu rms=%.4f dbfs=%.2f peak=%.4f\r\n",
                    (unsigned long)s_win_count, (double)rms, (double)dbfs, (double)s_win_peak);

            /* reset okna */
            s_win_count  = 0u;
            s_win_sum_sq = 0.0;
            s_win_peak   = 0.0;
        }
    }

    return MIC_ERR_OK;
}

/* =====================================================================================
 * PUBLIC API (driver)
 * ===================================================================================== */

void MIC_SetDebug(uint8_t enable)
{
    s_debug = (enable != 0u) ? 1u : 0u;
}

void MIC_Init(void)
{
    /*
     * Všetko je "softvérové" (HAL handle je už inicializovaný v MX_SPI1_Init()).
     * Tu si len nastavíme internal state.
     */
    s_inited = 1u;
    s_running = 0u;
    s_last_err = MIC_ERR_NOT_INIT;
    s_last_err_msg = "not started";
    s_dma_t0_ms = 0u;
    s_capture_t0_ms = 0u;

    s_win_count  = 0u;
    s_win_sum_sq = 0.0;
    s_win_peak   = 0.0;
    s_decim_phase = 0u;

    s_last_dbfs = -120.0f;
    s_last_rms  = 0.0f;

    /* debug default off */
    /* s_debug zostane ako bolo nastavené pred init (ak by si ho nastavil skôr) */

    s_interval_active = 0u;
    s_interval_t0_ms  = 0u;
    s_have_last_dma   = 0u;
    s_last_dma_words  = MIC_DMA_WORDS;

    s_capture_t0_ms = s_dma_t0_ms;
    pdm2pcm_reset();

    MIC_DBG("[MIC] Init done. Window=%ums, samples=%u, decim=%u\r\n",
            (unsigned)MIC_WINDOW_MS, (unsigned)MIC_WINDOW_SAMPLES, (unsigned)MIC_DECIM_N);
}

mic_err_t MIC_Start(void)
{
    if (!s_inited)
    {
        set_error(MIC_ERR_NOT_INIT, "ERROR: MIC_Start called before MIC_Init");
        return MIC_ERR_NOT_INIT;
    }

    if (s_running)
        return MIC_ERR_OK;

    /* Skús spustiť prvý DMA blok */
    mic_err_t e = start_dma_block();
    if (e != MIC_ERR_OK)
        return e;

    /*
     * Pri každom novom meraní (continuous aj interval) resetujeme PDM->PCM filter,
     * aby sa výsledok neviezol na starom stave (najmä v powersave režime).
     */
    pdm2pcm_reset();

    s_running = 1u;

    /* interval mode setup */
    if (mic_get_target_ms() != 0u)
    {
        s_interval_active = 1u;
        s_interval_t0_ms  = HAL_GetTick();

        /* reset accumulator - počítame RMS len za interval */
        s_win_count  = 0u;
        s_win_sum_sq = 0.0;
        s_win_peak   = 0.0;
        s_decim_phase = 0u;

        /* "neplatný" výsledok, kým interval neskončí */
        s_last_rms  = 0.0f;
        s_last_dbfs = -120.0f;
        s_last_err  = MIC_ERR_NO_DATA_YET;
        s_last_err_msg = "no data yet";

        MIC_DBG("[MIC] PowerSave compile-time: capture %lu ms then stop\r\n", (unsigned long)mic_get_target_ms());
    }
    else
    {
        s_interval_active = 0u;
        s_last_err = MIC_ERR_NO_DATA_YET;
        s_last_err_msg = "no data yet";
    }

    return MIC_ERR_OK;
}

void MIC_Stop(void)
{
    if (!s_running)
    {
        s_interval_active = 0u;
        return;
    }

    (void)HAL_SPI_Abort(&hspi1);
    s_running = 0u;
    s_interval_active = 0u;
}

void MIC_Task(void)
{
    /*
     * Táto funkcia má byť volaná často (v main while).
     *
     * POŽIADAVKA (strict):
     *  - MIC_PowerSaveMs = 0   -> SPI beží stále (t.j. po zavolaní MIC_Start beží bez prestávky)
     *  - MIC_PowerSaveMs = 10..100 -> SPI beží len počas merania (interval) a potom sa zastaví.
     *
     * Preto tu NEROBÍME žiadny automatický "continuous autostart".
     * Spustenie v powersave režime robí MIC_GetLast50ms() (keď príde príkaz AUDIO).
     */
    if (!s_inited)
        return;

    if (!s_running)
        return;

    /* Ak bola SPI chyba, zastav a nahlás */
    if (s_spi_err)
    {
        set_error(MIC_ERR_SPI_ERROR, "ERROR: SPI error during capture");
        MIC_Stop();
        return;
    }

    /* Ak DMA ešte nedobehlo, nič nerob */
    if (!s_spi_done)
    {
        if ((HAL_GetTick() - s_dma_t0_ms) > MIC_TIMEOUT_MS)
        {
            set_error(MIC_ERR_TIMEOUT, "ERROR: DMA timeout waiting for RxCplt");
            MIC_Stop();
        }
        return;
    }

    /* DMA dobehlo -> spracuj blok */
    (void)process_block_and_update_window();

    /*
     * POWERSAVE režim (MIC_PowerSaveMs 1..100):
     *  - ak čas ešte neuplynul -> pokračuj ďalším DMA blokom
     *  - ak čas už uplynul -> nižšie vetva "interval done" spraví RMS výpočet a MIC_Stop()
     */
    if (s_interval_active)
    {
        uint32_t elapsed = HAL_GetTick() - s_interval_t0_ms;
        uint32_t target  = mic_get_target_ms();

        if (elapsed < target)
        {
            (void)start_dma_block();
            return;
        }
        /* elapsed >= target -> neštartuj ďalší DMA, nechaj to dobehnúť do MIC_Stop() */
    }

    /* CONTINUOUS: vždy pokračuj ďalším DMA blokom */
    if (mic_get_target_ms() == 0u)
    {
        (void)start_dma_block();
        return;
    }

    /* ===== interval-finish (elapsed>=target) ===== */
    if (s_interval_active)
    {
        uint32_t elapsed = HAL_GetTick() - s_interval_t0_ms;
        uint32_t target  = mic_get_target_ms();

        if (elapsed >= target)
        {
            if (s_win_count == 0u)
            {
                set_error(MIC_ERR_TIMEOUT, "ERROR: interval finished but no samples collected");
                MIC_Stop();
                s_interval_active = 0u;
                return;
            }

            float rms = (float)sqrt(s_win_sum_sq / (double)s_win_count);
            float dbfs = safe_dbfs_from_rms(rms);

            if (rms > 0.98f || s_win_peak > 0.98)
            {
                set_error(MIC_ERR_SIGNAL_SATURATED, "ERROR: signal saturated (interval RMS/PEAK ~ 1.0)");
                MIC_Stop();
                s_interval_active = 0u;
                return;
            }

            s_last_rms  = rms;
            s_last_dbfs = dbfs;
            s_last_err  = MIC_ERR_OK;
            s_last_err_msg = NULL;

            MIC_DBG("[MIC] Interval %lums done: n=%lu rms=%.4f dbfs=%.2f\r\n",
                    (unsigned long)target,
                    (unsigned long)s_win_count,
                    (double)rms,
                    (double)dbfs);

            MIC_Stop();
            s_interval_active = 0u;
            return;
        }
    }
}

mic_err_t MIC_GetLast50ms(float *out_dbfs, float *out_rms)
{
    /*
     * V powersave režime (MIC_POWERSAVE != 0):
     *  - keď príde príkaz AUDIO a meranie ešte nebeží, driver sám spustí MIC_Start().
     */
    if (s_inited && (mic_get_target_ms() != 0u) && (!s_running) && (!s_interval_active))
    {
        (void)MIC_Start();
    }

    if (out_dbfs) *out_dbfs = (s_last_err == MIC_ERR_OK) ? s_last_dbfs : 0.0f;
    if (out_rms)  *out_rms  = (s_last_err == MIC_ERR_OK) ? s_last_rms  : 0.0f;
    return s_last_err;
}

/* =====================================================================================
 * LEGACY API (jednorazové meranie) – aby existujúci CLI fungoval
 * ===================================================================================== */

float MIC_LastDbFS(void) { return s_last_dbfs; }
float MIC_LastRms(void)  { return s_last_rms; }
const char* MIC_LastErrorMsg(void) { return s_last_err_msg; }

float MIC_ReadDbFS(void)
{
    return MIC_ReadDbFS_Debug();
}

float MIC_ReadDbFS_Debug(void)
{
    /*
     * Pre kompatibilitu: spravíme jednorazové meranie zhruba tak,
     * že zapneme debug, spustíme driver, počkáme na prvé hotové 50ms okno,
     * a potom driver zastavíme.
     */
    uint8_t prev_dbg = s_debug;
    s_debug = 1u;

    if (!s_inited)
        MIC_Init();

    mic_err_t e = MIC_Start();
    if (e != MIC_ERR_OK)
    {
        s_debug = prev_dbg;
        return -1.0f;
    }

    uint32_t t0 = HAL_GetTick();
    while (1)
    {
        MIC_Task();
        if (s_last_err == MIC_ERR_OK)
        {
            /* Máme aspoň jedno okno hotové */
            break;
        }

        if (s_last_err != MIC_ERR_NO_DATA_YET)
        {
            MIC_Stop();
            s_debug = prev_dbg;
            return -1.0f;
        }

        if ((HAL_GetTick() - t0) > 600u)
        {
            set_error(MIC_ERR_TIMEOUT, "ERROR: timeout waiting for 50ms window");
            MIC_Stop();
            s_debug = prev_dbg;
            return -1.0f;
        }
    }

    float out = s_last_dbfs;
    MIC_Stop();
    s_debug = prev_dbg;
    return out;
}
