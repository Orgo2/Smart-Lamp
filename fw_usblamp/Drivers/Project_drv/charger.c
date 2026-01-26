/*
 * charger.c - STNS01 charger control and status helpers.
 * Implements a simple periodic state machine.
 */

#include "charger.h"
#include "main.h"
#include "analog.h"
#include "lp_delay.h"
#include <stdio.h>
#include <stdarg.h>

/* State machine states */
static uint8_t charge_enabled = 0;
static uint32_t last_check_tick = 0;
static uint8_t cutoff_active = 0;
static GPIO_PinState sta_cached = GPIO_PIN_SET;
static GPIO_PinState sta_prev = GPIO_PIN_SET;
static uint32_t sta_sample_tick = 0;
static uint32_t sta_window_start_tick = 0;
static uint8_t sta_changes_in_window = 0;
static uint8_t sta_fault = 0;
static uint8_t s_lowbatt_enable_once = 0;

void CHARGER_Init(void)
{
    /* CTL_CEN already initialized as GPIO output in main.c */
    /* STA_CHG already initialized as GPIO input in main.c */
    /* LED already initialized as GPIO output in main.c */
    
    /* Start with charging disabled (policy decides when to enable). */
    HAL_GPIO_WritePin(CTL_CEN_GPIO_Port, CTL_CEN_Pin, GPIO_PIN_RESET);
    charge_enabled = 0;
    cutoff_active = 0;
    sta_cached = HAL_GPIO_ReadPin(STA_CHG_GPIO_Port, STA_CHG_Pin);
    sta_prev = sta_cached;
    sta_sample_tick = HAL_GetTick();
    sta_window_start_tick = sta_sample_tick;
    sta_changes_in_window = 0;
    sta_fault = 0;
    s_lowbatt_enable_once = 0;

    /* Force an early evaluation once ANALOG produces its first reading. */
    last_check_tick = 0;
}

void CHARGER_Task(void)
{
    uint32_t now = HAL_GetTick();

    /* Never keep the charger enabled on battery-only. */
    if (USB_IsPresent() == 0u)
    {
        if (charge_enabled)
        {
            HAL_GPIO_WritePin(CTL_CEN_GPIO_Port, CTL_CEN_Pin, GPIO_PIN_RESET);
            charge_enabled = 0;
        }
        cutoff_active = 0;
        sta_fault = 0;
        sta_changes_in_window = 0;
        sta_window_start_tick = now;
        sta_sample_tick = now;
        s_lowbatt_enable_once = 0;
        return;
    }

    /* Sample STA_CHG at ~2 Hz; fault is 1 Hz blinking so this reliably catches it. */
    if ((uint32_t)(now - sta_sample_tick) >= CHARGER_STA_SAMPLE_MS)
    {
        sta_sample_tick = now;
        sta_cached = HAL_GPIO_ReadPin(STA_CHG_GPIO_Port, STA_CHG_Pin);

        if ((uint32_t)(now - sta_window_start_tick) >= CHARGER_STA_WINDOW_MS)
        {
            sta_window_start_tick = now;
            sta_changes_in_window = 0;
            sta_fault = 0;
            sta_prev = sta_cached;
        }

        if (sta_cached != sta_prev)
        {
            sta_prev = sta_cached;
            if (sta_changes_in_window < 255u) sta_changes_in_window++;
            if (sta_changes_in_window >= 2u) sta_fault = 1u;
        }
    }
    
    /* Check battery voltage periodically */
    if ((now - last_check_tick) >= CHARGER_CHECK_INTERVAL_MS)
    {
        last_check_tick = now;

        /* Wait until ANALOG has produced at least one valid measurement. */
        if (ANALOG_GetUpdateId() == 0U)
        {
            return;
        }
        
        float vbat = ANALOG_GetBat();

        /* Voltage policy (requested):
         * - stop charging at >= 4.10V
         * - start charging only after it drops below 3.80V
         */
        if (vbat >= CHARGER_VBAT_STOP)
        {
            HAL_GPIO_WritePin(CTL_CEN_GPIO_Port, CTL_CEN_Pin, GPIO_PIN_RESET);
            charge_enabled = 0u;
            cutoff_active = 1u;
            return;
        }

        if (charge_enabled == 0u)
        {
            if (vbat <= CHARGER_VBAT_START)
            {
                if ((vbat < CHARGER_VBAT_MIN_START) && (s_lowbatt_enable_once == 0u))
                {
                    /* Safety lockout: do not start charging below VBAT_MIN_START unless manually enabled once. */
                    HAL_GPIO_WritePin(CTL_CEN_GPIO_Port, CTL_CEN_Pin, GPIO_PIN_RESET);
                    charge_enabled = 0u;
                    cutoff_active = 0u;
                    return;
                }

                HAL_GPIO_WritePin(CTL_CEN_GPIO_Port, CTL_CEN_Pin, GPIO_PIN_SET);
                charge_enabled = 1u;
                cutoff_active = 0u;
                s_lowbatt_enable_once = 0u;
            }
            return;
        }

        /* charge_enabled == 1 and VBAT < 4.1V: keep charging until stop threshold. */
    }

    /* Mirror charger status LED only when USB is connected. */
    if (USB_IsPresent() != 0u)
        IND_LED_Set((sta_cached == GPIO_PIN_RESET) ? 1u : 0u);
    else
        IND_LED_Set(0u);
}

uint8_t CHARGER_IsCharging(void)
{
    return charge_enabled;
}

uint8_t CHARGER_GetStatus(void)
{
    if (USB_IsPresent() == 0u) return 0; /* no USB */
    if (sta_fault) return 3; /* error/fault */
    if (charge_enabled == 0u) return 0; /* disabled by MCU */
    
    /* STA_CHG is active-low open-drain:
     * - Low = charging active
     * - High Z (pulled high by external pullup) = not charging
     */
    if (sta_cached == GPIO_PIN_RESET)
    {
        return 1; /* Charging */
    }
    else
    {
        return 2; /* Not charging */
    }
}

void CHARGER_Reset(void)
{
    if (USB_IsPresent() == 0u)
    {
        HAL_GPIO_WritePin(CTL_CEN_GPIO_Port, CTL_CEN_Pin, GPIO_PIN_RESET);
        charge_enabled = 0;
        cutoff_active = 0;
        last_check_tick = 0;
        return;
    }

    /* If battery voltage is unknown, be conservative and do not enable the charger yet. */
    if (ANALOG_GetUpdateId() == 0U)
    {
        HAL_GPIO_WritePin(CTL_CEN_GPIO_Port, CTL_CEN_Pin, GPIO_PIN_RESET);
        charge_enabled = 0u;
        cutoff_active = 0u;
        last_check_tick = 0;
        return;
    }

    float vbat_now = ANALOG_GetBat();
    if ((vbat_now < CHARGER_VBAT_MIN_START) && (s_lowbatt_enable_once == 0u))
    {
        /* Safety lockout: do not start charging below VBAT_MIN_START unless manually enabled once. */
        HAL_GPIO_WritePin(CTL_CEN_GPIO_Port, CTL_CEN_Pin, GPIO_PIN_RESET);
        charge_enabled = 0u;
        cutoff_active = 0u;
        last_check_tick = 0;
        return;
    }

    /* Toggle CTL_CEN pin to reset charger fault conditions:
     * - Charging timeout (pre-charge, fast-charge)
     * - Battery voltage below VPRE after fast-charge started
     * - End-of-charge
     */
    HAL_GPIO_WritePin(CTL_CEN_GPIO_Port, CTL_CEN_Pin, GPIO_PIN_RESET);
    LP_DELAY(100);
    HAL_GPIO_WritePin(CTL_CEN_GPIO_Port, CTL_CEN_Pin, GPIO_PIN_SET);
    
    /* Apply policy immediately if a VBAT measurement exists, otherwise let CHARGER_Task decide. */
    if (ANALOG_GetUpdateId() != 0U)
    {
        float vbat = ANALOG_GetBat();
        if (vbat >= CHARGER_VBAT_STOP)
        {
            HAL_GPIO_WritePin(CTL_CEN_GPIO_Port, CTL_CEN_Pin, GPIO_PIN_RESET);
            charge_enabled = 0u;
            cutoff_active = 1u;
        }
        else if (vbat <= CHARGER_VBAT_START)
        {
            if ((vbat < CHARGER_VBAT_MIN_START) && (s_lowbatt_enable_once == 0u))
            {
                HAL_GPIO_WritePin(CTL_CEN_GPIO_Port, CTL_CEN_Pin, GPIO_PIN_RESET);
                charge_enabled = 0u;
                cutoff_active = 0u;
                last_check_tick = 0;
                return;
            }
            HAL_GPIO_WritePin(CTL_CEN_GPIO_Port, CTL_CEN_Pin, GPIO_PIN_SET);
            charge_enabled = 1u;
            cutoff_active = 0u;
            s_lowbatt_enable_once = 0u;
        }
        else
        {
            HAL_GPIO_WritePin(CTL_CEN_GPIO_Port, CTL_CEN_Pin, GPIO_PIN_RESET);
            charge_enabled = 0u;
            /* Not a "cutoff": just waiting until VBAT drops below 3.8V. */
            cutoff_active = 0u;
        }
    }

    last_check_tick = 0; /* force immediate re-evaluation */
}

void CHARGER_LowBattEnableOnce(void)
{
    s_lowbatt_enable_once = 1u;
    last_check_tick = 0u; /* apply quickly in CHARGER_Task() */
}

static void charger_writef(charger_write_fn_t write, const char *fmt, ...)
{
    if (!write || !fmt) return;
    char buf[220];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    write(buf);
}

static float charger_battery_percent_from_v(float vbat)
{
    /* Simple linear estimate: 3.0V=0%, 4.2V=100%. */
    const float v0 = 3.0f;
    const float v1 = 4.2f;
    if (vbat <= v0) return 0.0f;
    if (vbat >= v1) return 100.0f;
    return (vbat - v0) * 100.0f / (v1 - v0);
}

static const char *charger_state_str(uint8_t st)
{
    switch (st)
    {
        case 0: return "disabled";
        case 1: return "charging";
        case 2: return "charged";
        case 3: return "error";
        default: return "unknown";
    }
}

void CHARGER_WriteStatus(charger_write_fn_t write)
{
    if (!write) return;

    float vbat = ANALOG_GetBat();
    float pct = charger_battery_percent_from_v(vbat);
    uint8_t st = CHARGER_GetStatus();

    uint8_t usb = (USB_IsPresent() != 0u) ? 1u : 0u;
    uint8_t mcu_wants = (HAL_GPIO_ReadPin(CTL_CEN_GPIO_Port, CTL_CEN_Pin) == GPIO_PIN_SET) ? 1u : 0u;
    GPIO_PinState sta = HAL_GPIO_ReadPin(STA_CHG_GPIO_Port, STA_CHG_Pin);
    const char *sta_desc = (sta == GPIO_PIN_RESET) ? "LOW (charger is charging)" : "HIGH (charger is not charging)";

    const char *policy = "stop>=4.10V start<3.80V";
    const char *mcu_note = "";
    if (!usb) mcu_note = "no USB";
    else if (!mcu_wants && vbat >= CHARGER_VBAT_STOP) mcu_note = "MCU stopped charging (VBAT >= 4.10V)";
    else if (!mcu_wants && vbat > CHARGER_VBAT_START) mcu_note = "MCU waiting (VBAT must drop below 3.80V)";
    else if (mcu_wants && vbat <= CHARGER_VBAT_START) mcu_note = "MCU enabled (VBAT below 3.80V)";
    else if (mcu_wants && vbat < CHARGER_VBAT_STOP) mcu_note = "MCU enabled (charging until 4.10V)";

    charger_writef(
        write,
        "BAT=%.1f%% VBAT=%.2fV STATE=%s USB=%s MCU_requests_charging=%s (%s) Policy=%s\r\n"
        "CHARGER_STATUS_PIN=%s\r\n",
        (double)pct,
        (double)vbat,
        charger_state_str(st),
        usb ? "YES" : "NO",
        mcu_wants ? "YES" : "NO",
        mcu_note,
        policy,
        sta_desc);
}
