#include <stddef.h>
#include "main.h"
#include "stm32u0xx_hal.h"
#include "led.h"
#include <string.h>
#include <stdio.h>

// Dummy implementácia pre linker
void led_hw_status(char *buf, size_t len) {
  snprintf(buf, len, "not implemented");
}

extern TIM_HandleTypeDef htim2;
extern DMA_HandleTypeDef hdma_tim2_ch1;

/*
 * TIM2 @ 48MHz, ARR=59 => 60 ticks => 1.25us (800kHz)
 * CCR1 duty counts:
 *  - 0-bit high  ~0.29us => 14
 *  - 1-bit high  ~0.75us => 36
 */
#define bitHightimercount  (36u)
#define bitLowtimercount   (14u)

#define numberofpixels     (30u)
#define bytesperpixel      (4u)   // RGBW
#define bitsperpixel       (bytesperpixel * 8u)
#define dataslots          (numberofpixels * bitsperpixel)

// reset low >200us. One slot = 1.25us => need >=160 slots.
// give margin:
#define resetslots          (240u)

#define total_slots         (dataslots + resetslots)

// framebuffer: user API stores RGBW; we will transmit GRBW
static uint8_t rgbw_arr[numberofpixels * bytesperpixel] = {0};

// DMA buffer MUST match CubeMX DMA width.
// Your CubeMX screenshot: Peripheral=Word, Memory=Byte (wrong).
// FIX WITHOUT TOUCHING IOC: we make buffer Word and write Word values (14/36/0).
static uint32_t pwm_buffer[total_slots] = {0};

static inline void put_byte_msb(uint32_t *dst, uint8_t v)
{
  for (int i = 7; i >= 0; i--) {
    *dst++ = (v & (1u << i)) ? bitHightimercount : bitLowtimercount;
  }
}

// Nastaví hodnoty pre jeden pixel (uloženie do RAM: RGBW)
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
  for (uint8_t i = 0; i < numberofpixels; ++i) {
    led_set_RGBW(i, r, g, b, w);
  }
}

void led_set_all_RGB(uint8_t r, uint8_t g, uint8_t b)
{
  led_set_all_RGBW(r, g, b, 0);
}

/*
 * Build whole frame into pwm_buffer[] (Word values 14/36/0),
 * then start TIM2 PWM DMA.
 *
 * IMPORTANT: SK6812 RGBW expects GRBW order (MSB first).
 */
void led_render(void)
{
  uint32_t p = 0;
  // stop previous (safe)
  HAL_TIM_PWM_Stop_DMA(&htim2, TIM_CHANNEL_1);

  for (uint32_t i = 0; i < numberofpixels; i++)
  {
    uint8_t r = rgbw_arr[i * 4u + 0u];
    uint8_t g = rgbw_arr[i * 4u + 1u];
    uint8_t b = rgbw_arr[i * 4u + 2u];
    uint8_t w = rgbw_arr[i * 4u + 3u];

    // transmit GRBW (not RGBW)
    put_byte_msb(&pwm_buffer[p], g); p += 8u;
    put_byte_msb(&pwm_buffer[p], r); p += 8u;
    put_byte_msb(&pwm_buffer[p], b); p += 8u;
    put_byte_msb(&pwm_buffer[p], w); p += 8u;
  }

  // reset low
  for (uint32_t k = 0; k < resetslots; k++) {
    pwm_buffer[p++] = 0u;
  }

  // start new transfer
  HAL_TIM_PWM_Start_DMA(&htim2, TIM_CHANNEL_1, pwm_buffer, p);
}
