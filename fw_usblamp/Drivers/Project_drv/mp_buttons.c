/*
 * mp_buttons.c - Simple debounced button events (short/long press).
 *
 * Reads B1/B2/BL GPIOs (active-high) and exposes short/long press events.
 * This module does not start programs or interact with MiniPascal state.
 */

#include "mp_buttons.h"
#include "main.h"
#include "stm32u0xx_hal.h"

#define EVT_B1 (1u << 0)
#define EVT_B2 (1u << 1)
#define EVT_BL (1u << 2)

typedef struct
{
    uint8_t raw;
    uint8_t stable;
    uint32_t change_ms;
    uint32_t press_ms;
} mp_btn_t;

static mp_btn_t s_b1;
static mp_btn_t s_b2;
static mp_btn_t s_bl;

static uint8_t s_short_events;
static uint8_t s_long_events;

static uint8_t btn_update(mp_btn_t *b, uint8_t raw, uint32_t now_ms)
{
    if (!b) return 0u;
    if (raw != b->raw)
    {
        b->raw = raw;
        b->change_ms = now_ms;
    }

    if (((uint32_t)(now_ms - b->change_ms) >= MP_BTN_DEBOUNCE_MS) && (b->stable != b->raw))
    {
        b->stable = b->raw;
        if (b->stable)
            b->press_ms = now_ms;
        return 1u;
    }
    return 0u;
}

void MP_Buttons_Init(void)
{
    s_b1 = (mp_btn_t){0};
    s_b2 = (mp_btn_t){0};
    s_bl = (mp_btn_t){0};
    s_short_events = 0u;
    s_long_events = 0u;
}

static void push_short(mp_btn_id_t id)
{
    switch (id)
    {
        case MP_BTN_B1: s_short_events |= EVT_B1; break;
        case MP_BTN_B2: s_short_events |= EVT_B2; break;
        case MP_BTN_BL: s_short_events |= EVT_BL; break;
        default: break;
    }
}

static void push_long(mp_btn_id_t id)
{
    switch (id)
    {
        case MP_BTN_B1: s_long_events |= EVT_B1; break;
        case MP_BTN_B2: s_long_events |= EVT_B2; break;
        case MP_BTN_BL: s_long_events |= EVT_BL; break;
        default: break;
    }
}

static void handle_release(mp_btn_id_t id, mp_btn_t *b, uint32_t now_ms)
{
    if (!b || b->press_ms == 0u) return;

    uint32_t dur = (uint32_t)(now_ms - b->press_ms);
    if (dur >= MP_BTN_LONG_MS) push_long(id);
    else push_short(id);

    b->press_ms = 0u;
}

void MP_Buttons_Poll(uint32_t now_ms)
{
    uint8_t raw_b1 = (uint8_t)(HAL_GPIO_ReadPin(B1_GPIO_Port, B1_Pin) == GPIO_PIN_SET);
    uint8_t raw_b2 = (uint8_t)(HAL_GPIO_ReadPin(B2_GPIO_Port, B2_Pin) == GPIO_PIN_SET);
    uint8_t raw_bl = (uint8_t)(HAL_GPIO_ReadPin(BL_GPIO_Port, BL_Pin) == GPIO_PIN_SET);

    (void)btn_update(&s_b1, raw_b1, now_ms);
    (void)btn_update(&s_b2, raw_b2, now_ms);
    (void)btn_update(&s_bl, raw_bl, now_ms);

    if (!s_b1.stable) handle_release(MP_BTN_B1, &s_b1, now_ms);
    if (!s_b2.stable) handle_release(MP_BTN_B2, &s_b2, now_ms);
    if (!s_bl.stable) handle_release(MP_BTN_BL, &s_bl, now_ms);
}

mp_btn_id_t MP_Buttons_PopShort(void)
{
    uint8_t e = s_short_events;
    if (e & EVT_B1) { s_short_events = (uint8_t)(e & (uint8_t)~EVT_B1); return MP_BTN_B1; }
    if (e & EVT_B2) { s_short_events = (uint8_t)(e & (uint8_t)~EVT_B2); return MP_BTN_B2; }
    if (e & EVT_BL) { s_short_events = (uint8_t)(e & (uint8_t)~EVT_BL); return MP_BTN_BL; }
    return MP_BTN_NONE;
}

mp_btn_id_t MP_Buttons_PopLong(void)
{
    uint8_t e = s_long_events;
    if (e & EVT_B1) { s_long_events = (uint8_t)(e & (uint8_t)~EVT_B1); return MP_BTN_B1; }
    if (e & EVT_B2) { s_long_events = (uint8_t)(e & (uint8_t)~EVT_B2); return MP_BTN_B2; }
    if (e & EVT_BL) { s_long_events = (uint8_t)(e & (uint8_t)~EVT_BL); return MP_BTN_BL; }
    return MP_BTN_NONE;
}

