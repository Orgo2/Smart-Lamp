/*
 * led.h
 */

#ifndef PROJECT_DRV_LED_H_
#define PROJECT_DRV_LED_H_

#include "stm32u0xx_hal.h"
#include <stdint.h>

void led_set_RGB(uint8_t index, uint8_t r, uint8_t g, uint8_t b);
void led_set_RGBW(uint8_t index, uint8_t r, uint8_t g, uint8_t b, uint8_t w);
void led_set_all_RGB(uint8_t r, uint8_t g, uint8_t b);
void led_set_all_RGBW(uint8_t r, uint8_t g, uint8_t b, uint8_t w);

void led_on(uint8_t r, uint8_t g, uint8_t b, uint8_t w);


void led_render();

// Vrati HW status LED drivera do buf (pre USB CLI)
void led_hw_status(char *buf, size_t len);

#endif
