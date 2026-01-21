/*
 * charger.c - STNS01 charger control and status helpers.
 * Implements a simple periodic state machine.
 */

#include "charger.h"
#include "main.h"
#include "analog.h"
#include "lp_delay.h"

/* State machine states */
static uint8_t charge_enabled = 0;
static uint32_t last_check_tick = 0;

/* Check battery voltage periodically */
#define CHARGER_CHECK_INTERVAL_MS  1000

void CHARGER_Init(void)
{
    /* CTL_CEN already initialized as GPIO output in main.c */
    /* STA_CHG already initialized as GPIO input in main.c */
    /* LED already initialized as GPIO output in main.c */
    
    /* Start with charging disabled */
    HAL_GPIO_WritePin(CTL_CEN_GPIO_Port, CTL_CEN_Pin, GPIO_PIN_RESET);
    charge_enabled = 0;
    
    last_check_tick = HAL_GetTick();
}

void CHARGER_Task(void)
{
    uint32_t now = HAL_GetTick();
    
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

        /* Voltage-based charge control with hysteresis (no USB stack control here). */
        if (vbat > CHARGER_VBAT_STOP && charge_enabled)
        {
            /* Stop charging - voltage too high */
            HAL_GPIO_WritePin(CTL_CEN_GPIO_Port, CTL_CEN_Pin, GPIO_PIN_RESET);
            charge_enabled = 0;
        }
        else if (vbat < CHARGER_VBAT_START && !charge_enabled)
        {
            /* Start charging - voltage low enough */
            HAL_GPIO_WritePin(CTL_CEN_GPIO_Port, CTL_CEN_Pin, GPIO_PIN_SET);
            charge_enabled = 1;
        }
    }
    
    /* Mirror charger status LED only when USB is connected; on battery this LED is used as a status LED. */
    if (USB_IsPresent() != 0u)
    {
        GPIO_PinState sta_chg = HAL_GPIO_ReadPin(STA_CHG_GPIO_Port, STA_CHG_Pin);
        /* STA_CHG is active-low: LED on when charging. */
        IND_LED_Set((sta_chg == GPIO_PIN_RESET) ? 1u : 0u);
    }
}

uint8_t CHARGER_IsCharging(void)
{
    return charge_enabled;
}

uint8_t CHARGER_GetStatus(void)
{
    /* Read STA_CHG pin state multiple times to detect toggling (fault condition) */
    GPIO_PinState state1 = HAL_GPIO_ReadPin(STA_CHG_GPIO_Port, STA_CHG_Pin);
    LP_DELAY(100); /* Wait 100ms */
    GPIO_PinState state2 = HAL_GPIO_ReadPin(STA_CHG_GPIO_Port, STA_CHG_Pin);
    LP_DELAY(100); /* Wait 100ms */
    GPIO_PinState state3 = HAL_GPIO_ReadPin(STA_CHG_GPIO_Port, STA_CHG_Pin);
    
    /* Check if pin is toggling (fault condition) */
    if (state1 != state2 || state2 != state3)
    {
        return 3; /* ERR - toggling at 1 Hz indicates fault */
    }
    
    /* STA_CHG is active-low open-drain:
     * - Low = charging active
     * - High Z (pulled high by external pullup) = not charging
     */
    if (state1 == GPIO_PIN_RESET)
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
    /* Toggle CTL_CEN pin to reset charger fault conditions:
     * - Charging timeout (pre-charge, fast-charge)
     * - Battery voltage below VPRE after fast-charge started
     * - End-of-charge
     */
    HAL_GPIO_WritePin(CTL_CEN_GPIO_Port, CTL_CEN_Pin, GPIO_PIN_RESET);
    LP_DELAY(100);
    HAL_GPIO_WritePin(CTL_CEN_GPIO_Port, CTL_CEN_Pin, GPIO_PIN_SET);
    
    /* Update internal state */
    charge_enabled = 1;
}
