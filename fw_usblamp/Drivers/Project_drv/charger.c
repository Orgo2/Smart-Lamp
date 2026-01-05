/*
 * charger.c - Battery charger management for STNS01
 *
 *  Created on: Jan 2, 2026
 *      Author: orgo
 */

#include "charger.h"
#include "main.h"
#include "analog.h"
#include "ux_api.h"

/* State machine states */
static uint8_t charge_enabled = 0;
static uint32_t last_check_tick = 0;
static uint8_t usb_disconnected = 0;

/* Check battery voltage every 100ms */
#define CHARGER_CHECK_INTERVAL_MS  100

void CHARGER_Init(void)
{
    /* CTL_CEN already initialized as GPIO output in main.c */
    /* STA_CHG already initialized as GPIO input in main.c */
    /* LED already initialized as GPIO output in main.c */
    
    /* Start with charging disabled */
    HAL_GPIO_WritePin(CTL_CEN_GPIO_Port, CTL_CEN_Pin, GPIO_PIN_RESET);
    charge_enabled = 0;
    usb_disconnected = 0;
    
    last_check_tick = HAL_GetTick();
}

void CHARGER_Task(void)
{
    uint32_t now = HAL_GetTick();
    
    /* Check battery voltage periodically */
    if ((now - last_check_tick) >= CHARGER_CHECK_INTERVAL_MS)
    {
        last_check_tick = now;
        
        float vbat = ANALOG_GetBat();
        
        /* Critical low voltage - disconnect USB to save power */
        if (vbat < CHARGER_VBAT_CRITICAL && !usb_disconnected)
        {
            /* Disconnect USB stack immediately */
            ux_device_stack_disconnect();
            usb_disconnected = 1;
            
            /* Enable charging before entering low power mode */
            HAL_GPIO_WritePin(CTL_CEN_GPIO_Port, CTL_CEN_Pin, GPIO_PIN_SET);
            charge_enabled = 1;
        }
        /* Voltage recovery - reset MCU if USB is connected */
        else if (vbat > CHARGER_VBAT_RECOVERY && usb_disconnected)
        {
            /* Check if USB cable is physically connected (VBUS present) */
            GPIO_PinState usb_vbus = HAL_GPIO_ReadPin(USB_GPIO_Port, USB_Pin);
            
            if (usb_vbus == GPIO_PIN_SET)
            {
                /* USB cable connected - reset MCU to reinitialize USB stack */
                NVIC_SystemReset();
            }
        }
        /* Normal voltage-based charge control with hysteresis */
        else if (!usb_disconnected)
        {
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
    }
    
    /* Update LED to mirror STA_CHG pin (charging status indicator) */
    GPIO_PinState sta_chg = HAL_GPIO_ReadPin(STA_CHG_GPIO_Port, STA_CHG_Pin);
    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, sta_chg);
}

uint8_t CHARGER_IsCharging(void)
{
    return charge_enabled;
}

uint8_t CHARGER_GetStatus(void)
{
    /* Read STA_CHG pin state multiple times to detect toggling (fault condition) */
    GPIO_PinState state1 = HAL_GPIO_ReadPin(STA_CHG_GPIO_Port, STA_CHG_Pin);
    HAL_Delay(100); /* Wait 100ms */
    GPIO_PinState state2 = HAL_GPIO_ReadPin(STA_CHG_GPIO_Port, STA_CHG_Pin);
    HAL_Delay(100); /* Wait 100ms */
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
    HAL_Delay(100);
    HAL_GPIO_WritePin(CTL_CEN_GPIO_Port, CTL_CEN_Pin, GPIO_PIN_SET);
    
    /* Update internal state */
    charge_enabled = 1;
}
