/*
 * lp_delay.h - Low-power delay helper.
 *
 * Uses normal HAL_Delay while USB is connected.
 * On battery, uses light sleep for short waits and STOP2+RTC WUT for longer waits.
 */

#pragma once

#include <stdint.h>

void LP_DELAY(uint32_t ms);

