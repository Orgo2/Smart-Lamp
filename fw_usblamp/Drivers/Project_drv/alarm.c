/*
 * alarm.c - beeper driver pre LPTIM2, kanál 1
 *
 * LPTIM2 config requirements (CubeMX / main.c):
 *   - Instance = LPTIM2
 *   - Clock.Source = LPTIM_CLOCKSOURCE_APBCLOCK_LPOSC (internal kernel clock)
 *   - Kernel clock = HSI16 (16 MHz) - set in HAL_LPTIM_MspInit via RCC
 *   - Prescaler = LPTIM_PRESCALER_DIV16 => tick = 1 MHz
 *
 * PA4 must be configured as AF14 (LPTIM2_CH1) - see HAL_LPTIM_MspPostInit.
 * NVIC TIM7_LPTIM2_IRQn must be enabled; ISR must call HAL_LPTIM_IRQHandler(&hlptim2).
 */

#include "alarm.h"
#include "main.h"

extern LPTIM_HandleTypeDef hlptim2;

#define ALARM_LPTIM   (&hlptim2)
#define ALARM_CHANNEL LPTIM_CHANNEL_1

/* LPTIM2 effective frequency after prescaler: HSI16/16 = 1 MHz */
#define ALARM_LPTIM_CLK_HZ 1000000u

static volatile uint32_t s_alarm_remaining_periods = 0;
static volatile uint8_t  s_alarm_active = 0;

/* ISR-safe deferral */
static volatile uint8_t  s_alarm_pending = 0;
static uint16_t s_alarm_req_freq_hz;
static uint8_t  s_alarm_req_volume;
static float    s_alarm_req_time_s;

static void alarm_stop(void)
{
    s_alarm_active = 0;
    s_alarm_remaining_periods = 0;
    (void)HAL_LPTIM_PWM_Stop_IT(ALARM_LPTIM, ALARM_CHANNEL);
}

static void alarm_stop_from_isr(void)
{
    s_alarm_active = 0;
    s_alarm_remaining_periods = 0;
    (void)HAL_LPTIM_PWM_Stop_IT(ALARM_LPTIM, ALARM_CHANNEL);
}

static void alarm_start_it(uint16_t freq_hz, uint8_t volume, float time_s)
{
    if (freq_hz == 0u)
        return;

    if (volume > 50u)
        volume = 50u;

    if (time_s <= 0.0f || volume == 0u)
    {
        alarm_stop();
        return;
    }

    /* Stop any previous tone */
    (void)HAL_LPTIM_PWM_Stop_IT(ALARM_LPTIM, ALARM_CHANNEL);

    /*
     * Reset HAL state to allow reconfiguration.
     * HAL checks State and ChannelState before operations.
     */
    ALARM_LPTIM->State = HAL_LPTIM_STATE_READY;
    ALARM_LPTIM->ChannelState[0] = HAL_LPTIM_CHANNEL_STATE_READY;

    /*
     * ticks = round(fclk / freq), fclk = 1 MHz
     * ARR = ticks - 1  (16-bit, max 0xFFFF)
     * 
     * Datasheet formula: duty_cycle = (CCR / ARR) * 100 %
     * So for volume% duty: CCR = ARR * volume / 100
     */
    const uint32_t f = (uint32_t)freq_hz;
    uint32_t ticks = (ALARM_LPTIM_CLK_HZ + (f / 2u)) / f;

    if (ticks < 2u) ticks = 2u;
    if (ticks > 65536u) ticks = 65536u;

    const uint32_t arr = ticks - 1u;

    /* CCR = ARR * volume / 100 => duty = CCR/ARR = volume% */
    uint32_t pulse = (arr * (uint32_t)volume) / 100u;
    if (pulse == 0u && volume > 0u) pulse = 1u;
    if (pulse > arr) pulse = arr;

    /* Update ARR via HAL - this also re-inits LPTIM with correct settings */
    ALARM_LPTIM->Init.Period = arr;
    if (HAL_LPTIM_Init(ALARM_LPTIM) != HAL_OK)
        return;

    /* Configure pulse (CCR) */
    LPTIM_OC_ConfigTypeDef oc = {0};
    oc.Pulse      = pulse;
    oc.OCPolarity = LPTIM_OCPOLARITY_HIGH;
    if (HAL_LPTIM_OC_ConfigChannel(ALARM_LPTIM, &oc, ALARM_CHANNEL) != HAL_OK)
        return;

    /* Calculate number of periods for the requested duration */
    if (time_s > 3600.0f) time_s = 3600.0f;
    uint32_t duration_ms = (uint32_t)(time_s * 1000.0f + 0.5f);

    uint64_t periods64 = ((uint64_t)f * (uint64_t)duration_ms + 999u) / 1000u;
    if (periods64 == 0u) periods64 = 1u;
    if (periods64 > 0xFFFFFFFFu) periods64 = 0xFFFFFFFFu;

    s_alarm_remaining_periods = (uint32_t)periods64;
    s_alarm_active = 1u;

    /* Start PWM with interrupts for auto-stop via ARRM callback */
    if (HAL_LPTIM_PWM_Start_IT(ALARM_LPTIM, ALARM_CHANNEL) != HAL_OK)
    {
        s_alarm_active = 0u;
        return;
    }
}

/* HAL callback - musí byť povolený LPTIM2 IRQ + HAL_LPTIM_IRQHandler(&hlptim2) v ISR */
void HAL_LPTIM_AutoReloadMatchCallback(LPTIM_HandleTypeDef *hlptim)
{
    if (hlptim != ALARM_LPTIM) return;
    if (!s_alarm_active) return;

    if (s_alarm_remaining_periods > 0u)
        s_alarm_remaining_periods--;

    if (s_alarm_remaining_periods == 0u)
        alarm_stop_from_isr();
}

/**
 * @brief Spusti beep na LPTIM2, CH1
 * @param freq_hz Frekvencia v Hz
 * @param volume Duty 0-50 (%) (50% = max)
 * @param time_s Trvanie v s
 */
void BEEP(uint16_t freq_hz, uint8_t volume, float time_s)
{
    /* If called from interrupt context, only stash params; run from main loop via BEEP_Task(). */
    if (__get_IPSR() != 0u)
    {
        s_alarm_req_freq_hz = freq_hz;
        s_alarm_req_volume  = volume;
        s_alarm_req_time_s  = time_s;
        s_alarm_pending = 1u;
        return;
    }

    alarm_start_it(freq_hz, volume, time_s);
}

void BEEP_Task(void)
{
    if (s_alarm_pending == 0u)
        return;

    s_alarm_pending = 0u;
    alarm_start_it(s_alarm_req_freq_hz, s_alarm_req_volume, s_alarm_req_time_s);
}

void BEEP_Test1kHz(void)
{
    const uint16_t freq_hz = 1000u;
    const uint8_t volume = 50u;

    const uint32_t f = (uint32_t)freq_hz;
    uint32_t ticks = (ALARM_LPTIM_CLK_HZ + (f / 2u)) / f;
    if (ticks < 2u) ticks = 2u;
    if (ticks > 65536u) ticks = 65536u;

    const uint32_t arr = ticks - 1u;
    /* CCR = ARR * volume / 100 => duty = CCR/ARR = volume% */
    uint32_t pulse = (arr * (uint32_t)volume) / 100u;
    if (pulse == 0u && volume > 0u) pulse = 1u;
    if (pulse > arr) pulse = arr;

    /* Stop any previous */
    (void)HAL_LPTIM_PWM_Stop(ALARM_LPTIM, ALARM_CHANNEL);

    /* Reset HAL state */
    ALARM_LPTIM->State = HAL_LPTIM_STATE_READY;
    ALARM_LPTIM->ChannelState[0] = HAL_LPTIM_CHANNEL_STATE_READY;

    /* Configure */
    ALARM_LPTIM->Init.Period = arr;
    (void)HAL_LPTIM_Init(ALARM_LPTIM);

    LPTIM_OC_ConfigTypeDef oc = {0};
    oc.Pulse      = pulse;
    oc.OCPolarity = LPTIM_OCPOLARITY_HIGH;
    (void)HAL_LPTIM_OC_ConfigChannel(ALARM_LPTIM, &oc, ALARM_CHANNEL);

    /* Start continuous PWM (no interrupts - for oscilloscope test only) */
    (void)HAL_LPTIM_PWM_Start(ALARM_LPTIM, ALARM_CHANNEL);
}
