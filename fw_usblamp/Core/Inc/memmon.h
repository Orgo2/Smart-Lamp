#pragma once

#include <stdint.h>

/* Initialize memory monitor (min-free tracking). */
void MemMon_Init(void);

/* Call periodically from main loop (updates heap end + timestamps). */
void MemMon_Task(void);

/* Call from SysTick hook (fast, no HAL/RTC calls). */
void MemMon_TickHook(void);

/* Snapshot for CLI/debug. min_dt is "HH:MM:SS_RR.MM.DD" or "N/A". */
void MemMon_Get(uint32_t *out_total, uint32_t *out_free, uint32_t *out_min_free,
                uint32_t *out_min_tick_ms, char *min_dt, uint32_t min_dt_sz);

