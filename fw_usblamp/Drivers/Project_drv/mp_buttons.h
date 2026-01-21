/*
 * mp_buttons.h - Simple debounced button events (short/long press).
 *
 * Produces release-based events:
 * - Short press event on release if held < long_ms.
 * - Long press event on release if held >= long_ms.
 */

#pragma once

#include <stdint.h>
////////////////////////////////button debounce and long press timing////////////////////////////
#define MP_BTN_DEBOUNCE_MS 30u
#define MP_BTN_LONG_MS     2000u

typedef enum
{
    MP_BTN_NONE = 0,
    MP_BTN_B1   = 1,
    MP_BTN_B2   = 2,
    MP_BTN_BL   = 3,
} mp_btn_id_t;

void MP_Buttons_Init(void);
void MP_Buttons_Poll(uint32_t now_ms);
mp_btn_id_t MP_Buttons_PopShort(void);
mp_btn_id_t MP_Buttons_PopLong(void);

