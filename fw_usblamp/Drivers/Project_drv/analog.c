/*
 * analog.c - Analog measurements (light sensor, battery, VCC).
 * Uses CubeMX-configured ADC1 in single-shot mode (no DMA).
 */

#include "analog.h"

#include "stm32u0xx_ll_adc.h"

static ADC_HandleTypeDef *s_hadc = NULL;

/* VDDA computed from VREFINT calibration. */
static float s_vdda_actual = ANALOG_VREF;

/*
 * Non-blocking sampling configuration
 * IMPORTANT:
 *  - ADC is configured by CubeMX with fixed sequencer (ScanConvMode = ADC_SCAN_SEQ_FIXED).
 *  - CubeMX may leave multiple channels enabled in CHSELR.
 *  - This driver forces CHSELR to exactly one channel before each conversion.
 *  - Only single-shot conversions are used (no DMA, no continuous mode).
 */
#ifndef ANALOG_VREF_SAMPLES
#define ANALOG_VREF_SAMPLES 4U
#endif

/* Cached results */
static volatile uint32_t s_raw_vrefint;
static volatile uint32_t s_raw_bat;
static volatile uint32_t s_raw_light;

static volatile float s_bat_v;
static volatile float s_light_lux;

typedef enum
{
    ANALOG_STEP_IDLE = 0,
    ANALOG_STEP_VREFINT,
    ANALOG_STEP_BAT_DUMMY,
    ANALOG_STEP_BAT,
    ANALOG_STEP_LIGHT,
} analog_step_t;

static volatile analog_step_t s_step = ANALOG_STEP_IDLE;
static volatile uint32_t s_accum = 0U;
static volatile uint32_t s_samples_left = 0U;
static volatile uint8_t s_busy = 0U;
static volatile uint8_t s_update_requested = 0U;
static volatile uint32_t s_update_id = 0U;
static volatile uint8_t s_conv_running = 0U;
static volatile uint32_t s_last_update_tick = 0U;

/* Private prototypes */
static void ANALOG_ConfigChannel(uint32_t channel);
static float ANALOG_CalcVddaFromVrefint(uint32_t vrefint_adc);
static void ANALOG_StartStep(analog_step_t step);

static float ANALOG_CalcVddaFromVrefint(uint32_t vrefint_adc)
{
    uint16_t vrefint_cal = *VREFINT_CAL_ADDR;
    if (vrefint_adc == 0U || vrefint_cal == 0U)
        return ANALOG_VREF;

    float vdda = ((float)VREFINT_CAL_VREF / 1000.0f) * ((float)vrefint_cal / (float)vrefint_adc);

    /* sanity */
    if (vdda < 2.0f || vdda > 3.6f)
        vdda = ANALOG_VREF;

    return vdda;
}

void ANALOG_Init(ADC_HandleTypeDef *hadc)
{
    s_hadc = hadc;

    (void)HAL_ADC_Stop(s_hadc);
    (void)HAL_ADCEx_Calibration_Start(s_hadc);

    s_last_update_tick = 0U;

    /* Start first background update (non-blocking) */
    ANALOG_RequestUpdate();
}

static void ANALOG_ConfigChannel(uint32_t channel)
{
    ADC_ChannelConfTypeDef sConfig = {0};

    sConfig.Channel = channel;
    /* main.c config: ScanConvMode = ADC_SCAN_SEQ_FIXED -> rank fixed by channel number */
    sConfig.Rank = ADC_RANK_CHANNEL_NUMBER;
    sConfig.SamplingTime = ADC_SAMPLINGTIME_COMMON_1;

    /*
     * Important on STM32U0 with fixed sequencer:
     * CubeMX often enables multiple channels in CHSELR.
     * In fixed sequencer mode, HAL_ADC_ConfigChannel() only ADDS channels
     * (does not remove others), causing conversions to scan multiple channels.
     * Then PollForConversion() returns the first rank repeatedly and all our
     * reads look identical.
     *
     * Force single-channel selection by overwriting CHSELR.
     */
    (void)HAL_ADC_ConfigChannel(s_hadc, &sConfig);
    s_hadc->Instance->CHSELR = (channel & ADC_CHSELR_CHSEL);
}

static void ANALOG_StartStep(analog_step_t step)
{
    if (s_hadc == NULL)
        return;

    s_step = step;
    s_accum = 0U;

    switch (step)
    {
        case ANALOG_STEP_VREFINT:
            s_samples_left = (uint32_t)ANALOG_VREF_SAMPLES;
            ANALOG_ConfigChannel(ADC_CHANNEL_VREFINT);
            break;

        case ANALOG_STEP_BAT_DUMMY:
            /* PB0/ADC1_IN17: dummy shot after channel switch (no scan used) */
            s_samples_left = 1U;
            ANALOG_ConfigChannel(ANALOG_BAT_CHANNEL);
            break;

        case ANALOG_STEP_BAT:
            s_samples_left = (uint32_t)ANALOG_NUM_SAMPLES;
            ANALOG_ConfigChannel(ANALOG_BAT_CHANNEL);
            break;

        case ANALOG_STEP_LIGHT:
            s_samples_left = (uint32_t)ANALOG_NUM_SAMPLES;
            ANALOG_ConfigChannel(ANALOG_LIGHT_CHANNEL);
            break;

        default:
            s_samples_left = 0U;
            break;
    }

    if (s_samples_left == 0U)
        return;

    /* Start single-shot conversion (non-blocking). */
    if (HAL_ADC_Start(s_hadc) != HAL_OK)
    {
        s_step = ANALOG_STEP_IDLE;
        s_busy = 0U;
        s_update_requested = 0U;
        s_conv_running = 0U;
        return;
    }
    s_conv_running = 1U;
}

void ANALOG_RequestUpdate(void)
{
    s_update_requested = 1U;
}

uint8_t ANALOG_IsBusy(void)
{
    return s_busy;
}

void ANALOG_Task(void)
{
    /* Start a new non-blocking sequence if requested */
    if ((s_hadc != NULL) && (s_busy == 0U))
    {
        uint32_t now = HAL_GetTick();
        uint8_t do_start = 0U;

        if (s_update_requested != 0U)
        {
            do_start = 1U;
        }
        else if (ANALOG_AUTO_UPDATE_MS != 0U)
        {
            if ((uint32_t)(now - s_last_update_tick) >= (uint32_t)ANALOG_AUTO_UPDATE_MS)
                do_start = 1U;
        }

        if (do_start)
        {
            s_busy = 1U;
            s_conv_running = 0U;
            s_update_requested = 0U;
            ANALOG_StartStep(ANALOG_STEP_VREFINT);
            return;
        }
    }

    /* Progress conversion state machine without blocking */
    if ((s_hadc != NULL) && (s_busy != 0U) && (s_conv_running != 0U))
    {
        HAL_StatusTypeDef st = HAL_ADC_PollForConversion(s_hadc, 0U);
        if (st == HAL_TIMEOUT)
        {
            return; /* not ready yet */
        }

        if (st != HAL_OK)
        {
            (void)HAL_ADC_Stop(s_hadc);
            s_step = ANALOG_STEP_IDLE;
            s_busy = 0U;
            s_conv_running = 0U;
            return;
        }

        uint32_t v = HAL_ADC_GetValue(s_hadc);
        (void)HAL_ADC_Stop(s_hadc);
        s_conv_running = 0U;

        /* Dummy shot for IN17 after channel switch */
        if (s_step == ANALOG_STEP_BAT_DUMMY)
        {
            ANALOG_StartStep(ANALOG_STEP_BAT);
            return;
        }

        s_accum += v;
        if (s_samples_left > 0U)
            s_samples_left--;

        if (s_samples_left > 0U)
        {
            /* next sample on same channel */
            if (HAL_ADC_Start(s_hadc) == HAL_OK)
                s_conv_running = 1U;
            else
            {
                s_step = ANALOG_STEP_IDLE;
                s_busy = 0U;
            }
            return;
        }

        /* Step finished: compute avg and transition */
        uint32_t avg = 0U;
        switch (s_step)
        {
            case ANALOG_STEP_VREFINT:
                avg = (uint32_t)(s_accum / (uint32_t)ANALOG_VREF_SAMPLES);
                s_raw_vrefint = avg;
                s_vdda_actual = ANALOG_CalcVddaFromVrefint(avg);
                ANALOG_StartStep(ANALOG_STEP_BAT_DUMMY);
                return;

            case ANALOG_STEP_BAT:
                avg = (uint32_t)(s_accum / (uint32_t)ANALOG_NUM_SAMPLES);
                s_raw_bat = avg;
                ANALOG_StartStep(ANALOG_STEP_LIGHT);
                return;

            case ANALOG_STEP_LIGHT:
                avg = (uint32_t)(s_accum / (uint32_t)ANALOG_NUM_SAMPLES);
                s_raw_light = avg;
                /* Finalize: compute corrected raws + derived values */
                {
                    float vdda = s_vdda_actual;

                    /* Battery voltage from raw using actual VDDA */
                    float v_adc = ((float)s_raw_bat / ANALOG_ADC_MAX_VALUE) * vdda;
                    float v_bat = 0.0f;
                    if (ANALOG_BAT_DIVIDER > 0.0f)
                        v_bat = v_adc / ANALOG_BAT_DIVIDER;
                    if (v_bat < 0.0f) v_bat = 0.0f;
                    if (v_bat > 5.0f) v_bat = 5.0f;
                    s_bat_v = v_bat;

                    /* Light: photodiode current -> lux */
                    float v_light = ((float)s_raw_light / ANALOG_ADC_MAX_VALUE) * vdda;
                    float current = v_light / ANALOG_TIA_RESISTOR;
                    float lux = 0.0f;
                    if (ANALOG_PD_A_PER_LUX > 0.0f)
                        lux = current / ANALOG_PD_A_PER_LUX;
                    if (lux < 0.0f) lux = 0.0f;
                    if (lux > 100000.0f) lux = 100000.0f;
                    s_light_lux = lux;

                    s_update_id++;
                    s_last_update_tick = HAL_GetTick();
                }

                s_step = ANALOG_STEP_IDLE;
                s_busy = 0U;
                return;

            default:
                s_step = ANALOG_STEP_IDLE;
                s_busy = 0U;
                return;
        }
    }
}

uint32_t ANALOG_GetUpdateId(void)
{
    return s_update_id;
}

float ANALOG_GetLight(void)
{
    return s_light_lux;
}

float ANALOG_GetBat(void)
{
    return s_bat_v;
}

float ANALOG_GetVcc(void)
{
    return s_vdda_actual;
}
