#pragma once
/*
  MiniPascal (tiny Pascal-like) interpreter for STM32 (HAL) with terminal UI over USB CDC (VCP).

  Features (per project requests):
  - 3 flash program slots inside linker FLASH_DATA region
  - SAVE <slot> compiles + erases slot + saves
  - LOAD <slot> loads + runs
  - Abort running program if abort-button held MP_ABORT_HOLD_MS (no MCU reset)
  - EDIT mode ends with the word QUIT
 */

#include <stdint.h>
#include <stdbool.h>

/* ---------------- Configuration (tune for your RAM/flash budget) ----------------
   Your MCU: STM32U073, RAM 40KB, free ~13KB; you requested program RAM ~6KB.
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

/* Variables:
   First SYSVAR_COUNT slots are reserved for system variables (CMDID, LEDR, TIMEH...)
   Remaining slots are for user variables.
*/
#ifndef MP_MAX_VARS
#define MP_MAX_VARS         40      /* total variable slots (sys + user) */
#endif

#ifndef MP_BC_MAX
#define MP_BC_MAX           1536    /* bytecode buffer; lower saves RAM */
#endif

#ifndef MP_STACK_SIZE
#define MP_STACK_SIZE       40      /* VM stack depth */
#endif

#ifndef MP_MAX_FIXUPS
#define MP_MAX_FIXUPS       48      /* forward goto fixups */
#endif

/* ---------------- Flash layout (STM32U073: 256KB flash, 2KB page) ----------------
   Program slots are stored inside the linker FLASH_DATA region:
     __flash_data_start__ .. __flash_data_end__ (see .ld)
   Slot size = region_size / MP_FLASH_SLOT_COUNT, aligned down to page size.
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

/* ---------------- HAL glue you must provide ---------------- */
/* Return -1 if no char available, else 0..255. Non-blocking recommended. */
int  mp_hal_getchar(void);
/* Send one char to terminal. */
void mp_hal_putchar(char c);
/* Milliseconds tick (HAL_GetTick). */
uint32_t mp_hal_millis(void);

/* Abort button (optional):
   Return 1 if pressed, 0 if not pressed.
   You can implement this to read PA2 (B2 button) with your GPIO config.
   If you don't implement it, default weak implementation returns 0.
*/
int mp_hal_abort_pressed(void);

/* ---------------- Builtins (you implement) ----------------
   Interpreter supports function calls: name(expr, expr, ...)

   Builtin IDs are defined by the name->id table in MiniPascal.c.
*/
int32_t mp_user_builtin(uint8_t id, uint8_t argc, const int32_t *argv);

/* Execute a single builtin call line: NAME(arg,...) -> returns true if handled. */
bool mp_exec_builtin_line(const char *line, int32_t *ret_out, bool *has_ret);

/* ---------------- Public API ---------------- */
void mp_init(void);
/* Call often from main loop; handles terminal + runs program time-sliced. */
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
void mp_buttons_poll(void);
void mp_autorun_poll(void);
/* Returns first non-empty slot (1..3) or 0 if all empty. */
uint8_t mp_first_program_slot(void);
