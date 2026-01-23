/*
 * led.c - SK6812 RGBW LED strip driver using TIM2 PWM + DMA.
 * Stores pixels as RGBW in RAM and transmits GRBW (MSB first) to the LEDs.
 */

#include "main.h"
#include "stm32u0xx_hal.h"
#include "led.h"

extern TIM_HandleTypeDef htim2;
extern DMA_HandleTypeDef hdma_tim2_ch1;

static volatile uint8_t s_led_dma_done = 1u;

void HAL_TIM_PWM_PulseFinishedCallback(TIM_HandleTypeDef *htim)
{
    if (htim != &htim2) return;
    s_led_dma_done = 1u;
}

/*
 * Timing: TIM2 @ 48 MHz, ARR=59 => 60 ticks => 1.25 us (800 kHz).
 * Duty (CCR1): 0-bit high ~0.29 us => 14, 1-bit high ~0.75 us => 36.
 */
#define bitHightimercount  (36u)
#define bitLowtimercount   (14u)

#define numberofpixels     (30u)
#define bytesperpixel      (4u)   /* RGBW */
#define bitsperpixel       (bytesperpixel * 8u)
#define dataslots          (numberofpixels * bitsperpixel)

/* Reset low >200 us. One slot = 1.25 us => need >=160 slots (use margin). */
#define resetslots          (240u)

#define total_slots         (dataslots + resetslots)

/* Framebuffer: user API stores RGBW; the wire order is GRBW. */
static uint8_t rgbw_arr[numberofpixels * bytesperpixel] = {0};

/* DMA buffer must match CubeMX DMA width (use 32-bit words for CCR values). */
static uint32_t pwm_buffer[total_slots] = {0};

static inline void put_byte_msb(uint32_t *dst, uint8_t v)
{
    for (int i = 7; i >= 0; i--)
    {
        *dst++ = (v & (1u << i)) ? bitHightimercount : bitLowtimercount;
    }
}

/* Set one pixel in the framebuffer (RGBW in RAM). */
void led_set_RGBW(uint8_t index, uint8_t r, uint8_t g, uint8_t b, uint8_t w)
{
    if (index >= numberofpixels) return;
    rgbw_arr[index * 4u + 0u] = r;
    rgbw_arr[index * 4u + 1u] = g;
    rgbw_arr[index * 4u + 2u] = b;
    rgbw_arr[index * 4u + 3u] = w;
}

void led_set_RGB(uint8_t index, uint8_t r, uint8_t g, uint8_t b)
{
    led_set_RGBW(index, r, g, b, 0);
}

void led_set_all_RGBW(uint8_t r, uint8_t g, uint8_t b, uint8_t w)
{
    for (uint8_t i = 0; i < numberofpixels; ++i)
        led_set_RGBW(i, r, g, b, w);
}

void led_set_all_RGB(uint8_t r, uint8_t g, uint8_t b)
{
    led_set_all_RGBW(r, g, b, 0);
}

/*
 * Build the full frame into pwm_buffer[] (word values 14/36/0),
 * then start the TIM2 PWM DMA transfer.
 *
 * IMPORTANT: SK6812 RGBW expects GRBW order (MSB first).
 */
void led_render(void)
{
    uint32_t p = 0;

    /* Stop any previous transfer. */
    HAL_TIM_PWM_Stop_DMA(&htim2, TIM_CHANNEL_1);
    s_led_dma_done = 1u;

    /* Encode pixels (GRBW, MSB first). */
    for (uint32_t i = 0; i < numberofpixels; i++)
    {
        uint8_t r = rgbw_arr[i * 4u + 0u];
        uint8_t g = rgbw_arr[i * 4u + 1u];
        uint8_t b = rgbw_arr[i * 4u + 2u];
        uint8_t w = rgbw_arr[i * 4u + 3u];

        put_byte_msb(&pwm_buffer[p], g); p += 8u;
        put_byte_msb(&pwm_buffer[p], r); p += 8u;
        put_byte_msb(&pwm_buffer[p], b); p += 8u;
        put_byte_msb(&pwm_buffer[p], w); p += 8u;
    }

    /* Append reset low time. */
    for (uint32_t k = 0; k < resetslots; k++)
        pwm_buffer[p++] = 0u;

    /* Start the new transfer. */
    s_led_dma_done = 0u;
    if (HAL_TIM_PWM_Start_DMA(&htim2, TIM_CHANNEL_1, pwm_buffer, p) != HAL_OK)
    {
        s_led_dma_done = 1u;
        return;
    }

    /* Wait for DMA transfer to complete before any deep sleep can occur. */
    uint32_t t0 = HAL_GetTick();
    while (!s_led_dma_done && ((uint32_t)(HAL_GetTick() - t0) < 10u))
        __WFI();

    /* Stop PWM+DMA after transfer (or timeout). */
    (void)HAL_TIM_PWM_Stop_DMA(&htim2, TIM_CHANNEL_1);
    s_led_dma_done = 1u;
}
