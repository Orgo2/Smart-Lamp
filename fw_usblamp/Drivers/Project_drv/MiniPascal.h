#pragma once
/*
 * MiniPascal - a small Pascal-like language for this lamp.
 * You edit text lines over USB, the firmware compiles them to bytecode, then runs them on a tiny VM.
 */

#include <stdint.h>
#include <stdbool.h>

/*
 * Configuration (RAM/flash budget).
 * Higher values allow bigger programs but use more RAM.
 */
#ifndef MP_MAX_LINES
#define MP_MAX_LINES        50      /* stored program lines */
#endif

#ifndef MP_LINE_LEN
#define MP_LINE_LEN         72      /* max chars per line (NUL included) */
#endif

#ifndef MP_NAME_LEN
#define MP_NAME_LEN         12      /* identifier length (NUL included) */
#endif

/*
 * Variables: the first SYSVAR_COUNT slots are system variables (TIME*, etc).
 * The remaining slots are for user variables.
 */
#ifndef MP_MAX_VARS
#define MP_MAX_VARS         40      /* total variable slots (sys + user) */
#endif

#ifndef MP_BC_MAX
#define MP_BC_MAX           1536    /* Bytecode buffer; lower saves RAM. */
#endif

#ifndef MP_STACK_SIZE
#define MP_STACK_SIZE       40      /* VM stack depth (expression evaluation). */
#endif

#ifndef MP_MAX_FIXUPS
#define MP_MAX_FIXUPS       48      /* forward goto fixups */
#endif

/*
 * Flash program storage (3 slots).
 * Slots live inside the linker FLASH_DATA region: __flash_data_start__ .. __flash_data_end__.
 */
#ifndef MP_FLASH_TOTAL_SIZE
#define MP_FLASH_TOTAL_SIZE (256u * 1024u)   /* bytes */
#endif

#ifndef MP_FLASH_PAGE_SIZE
#define MP_FLASH_PAGE_SIZE  (2048u)          /* bytes per page */
#endif

#ifndef MP_FLASH_SLOT_PAGES
#define MP_FLASH_SLOT_PAGES (4u)             /* pages per slot (4 pages => 8KB per program) */
#endif

#ifndef MP_FLASH_SLOT_COUNT
#define MP_FLASH_SLOT_COUNT (3u)
#endif

/* Abort button hold time (ms). Keep below reset-hold time if both use same pin. */
#ifndef MP_ABORT_HOLD_MS
#define MP_ABORT_HOLD_MS    (2000u)
#endif

/* HAL glue provided by the firmware/board layer. */
/* Return -1 if no char available, else 0..255. Non-blocking recommended. */
int  mp_hal_getchar(void);
/* Send one char to terminal. */
void mp_hal_putchar(char c);
/* Milliseconds tick (HAL_GetTick). */
uint32_t mp_hal_millis(void);

/* Return 1 if USB is present, 0 otherwise. */
int mp_hal_usb_connected(void);

/* LED power rail control (CTL_LEN). */
void mp_hal_led_power_on(void);
void mp_hal_led_power_off(void);

/* Low-power delay used on battery for longer waits. */
void mp_hal_lowpower_delay_ms(uint32_t ms);

/*
 * Abort button (optional): return 1 if pressed, 0 if not pressed.
 * If not implemented, the weak default returns 0.
 */
int mp_hal_abort_pressed(void);

/*
 * Builtins are C-backed functions callable from Pascal, e.g. LED(...), DELAY(ms), TIME(), etc.
 * Names are mapped to numeric IDs in MiniPascal.c and executed by mp_user_builtin().
 */
int32_t mp_user_builtin(uint8_t id, uint8_t argc, const int32_t *argv);

/* Execute a single builtin call line: NAME(arg,...) -> returns true if handled. */
bool mp_exec_builtin_line(const char *line, int32_t *ret_out, bool *has_ret);

/* ---------------- Public API ---------------- */
void mp_init(void);
/* Call often from main loop; handles terminal input and runs the program time-sliced. */
void mp_poll(void);
 /* Async stop (also available as terminal command STOP). */
 void mp_request_stop(void);
 /* Request program start from a slot (safe to call from IRQ). */
 void mp_request_run_slot(uint8_t slot);
 /* Request (re)start of the currently loaded program (safe to call from IRQ). */
 void mp_request_run_loaded(void);
 /* Notify interpreter that USB was detached (safe to call from IRQ). */
 void mp_request_usb_detach(void);
void mp_start_session(void);
void mp_stop_session(void);
void mp_feed_char(char c);
void mp_task(void);
bool mp_is_active(void);
bool mp_exit_pending(void);
void mp_autorun_poll(void);
/* Returns first non-empty slot (1..3) or 0 if all empty. */
uint8_t mp_first_program_slot(void);

/* Button events from the board layer (used on battery, outside USB session). */
void mp_notify_button_short(uint8_t btn_id);
void mp_notify_button_long(uint8_t btn_id);
