#include "usb_cli.h"

#include "main.h"
#include "ux_device_cdc_acm.h"
#include "rtc.h"
#include "charger.h"
#include "mic.h"
#include <MiniPascal.h>
#include "memmon.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>
#include <ctype.h>

#include "stm32u0xx_hal.h"

#ifndef USB_CLI_RX_CHUNK
#define USB_CLI_RX_CHUNK 64
#endif

#ifndef USB_CLI_LINE_MAX
#define USB_CLI_LINE_MAX 128
#endif

static char s_line[USB_CLI_LINE_MAX];
static uint32_t s_line_len;

static void cdc_writef(const char *fmt, ...);

static int cli_stricmp(const char *a, const char *b)
{
    while (*a && *b)
    {
        char ca = (char)tolower((unsigned char)*a++);
        char cb = (char)tolower((unsigned char)*b++);
        if (ca != cb) return (int)ca - (int)cb;
    }
    return (int)(unsigned char)*a - (int)(unsigned char)*b;
}

static bool cli_is_time0_call(const char *line)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!isalpha((unsigned char)*p) && *p != '_') return false;
    char name[16];
    uint8_t i = 0;
    while ((*p == '_' || isalnum((unsigned char)*p)) && i < (sizeof(name) - 1))
        name[i++] = *p++;
    name[i] = 0;
    if (cli_stricmp(name, "time") != 0) return false;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '(') return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ')') return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return (*p == 0);
}

static bool cli_is_call0(const char *line, const char *fname)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!isalpha((unsigned char)*p) && *p != '_') return false;
    char name[16];
    uint8_t i = 0;
    while ((*p == '_' || isalnum((unsigned char)*p)) && i < (sizeof(name) - 1))
        name[i++] = *p++;
    name[i] = 0;
    if (cli_stricmp(name, fname) != 0) return false;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '(') return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ')') return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    return (*p == 0);
}

static void dbg_led_blink(uint8_t times)
{
    /*
     * Visible error blink on HW: must be >= 100ms.
     * Keep it short so it doesn't disturb USB/CLI responsiveness too much.
     */
    const uint32_t on_ms  = 120;
    const uint32_t off_ms = 120;

    if (times > 2) times = 2;
    if (times == 0) return;

    for (uint8_t i = 0; i < times; i++)
    {
        IND_LED_On();
        HAL_Delay(on_ms);
        IND_LED_Off();
        HAL_Delay(off_ms);
    }
}

void cdc_write_str(const char *s)
{
    if (s == NULL) return;
    uint32_t sent = 0;

    /* Non-blocking: if TX not ready (return=2) or not connected, just drop. */
    (void)USBD_CDC_ACM_Transmit((uint8_t*)s, (uint32_t)strlen(s), &sent);
}

static void cdc_writef(const char *fmt, ...)
{
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cdc_write_str(buf);
}

static void cdc_echo_char(char c)
{
    uint32_t sent = 0;
    (void)USBD_CDC_ACM_Transmit((uint8_t*)&c, 1, &sent);
}

static void cdc_prompt(void)
{
    cdc_write_str("> ");
}

/* Pascal interpreter mode */
static uint8_t s_pascal_mode = 0;
static uint8_t s_usb_connected = 0;

void USB_CLI_NotifyDetach(void)
{
    s_usb_connected = 0;
    if (s_pascal_mode)
    {
        s_pascal_mode = 0;
        mp_stop_session();
    }
}

static void print_help(void)
{
    cdc_write_str(
        "COMMANDS:\r\n"
        "  HELP\r\n"
        "  PING\r\n"
        "  MEM         (RAM total/free/minfree)\r\n"
        "  PASCAL      (enter interpreter)\r\n"
        "  FINDMIC     (find working mic SPI mode)\r\n"
        "  MICDIAG     (mic SPI/DMA/pin diagnostics)\r\n"
        "  CHARGER     (battery %, state, VBAT)\r\n"
        "  CHGRST      (reset charger)\r\n"
        "  LOBATT_ENABLE (allow charging <1.7V once)\r\n"
        "\r\n"
        "PASCAL CALLS (same as interpreter):\r\n"
        "  LED(i,r,g,b,w)\r\n"
        "  LEDON(r,g,b,w)\r\n"
        "  LEDOFF()\r\n"
        "  DELAY(ms)\r\n"
        "  BATTERY()\r\n"
        "  LIGHT()\r\n"
        "  BTN()       (0=none, 1=B1, 2=B2, 3=BL)\r\n"
        "  RNG()\r\n"
        "  TEMP()\r\n"
        "  HUM()\r\n"
        "  PRESS()\r\n"
        "  MIC()\r\n"
        "  MICFFT()    (prints LF,MF,HF dBFS*100)\r\n"
        "            bands: LF=100-400 MF=400-1600 HF=1600-4000 Hz\r\n"
        "  TIME()      (prints YY,MO,DD,HH,MM)\r\n"
        "  TIME(sel)   (return part: 0=YY 1=MO 2=DD 3=HH 4=MM 5=SS)\r\n"
        "  SETTIME(yy,mo,dd,hh,mm)   (set date+time, sec=0)\r\n"
        "  SETTIME(hh,mm,ss)         (set time only, keep date)\r\n"
        "            yy=0..99 mo=1..12 dd=1..31 hh=0..23 mm=0..59 ss=0..59\r\n"
        "  ALARM()     (1 while alarm is running, else 0)\r\n"
        "  SETALARM(hh,mm[,dur])     (daily alarm, dur seconds, 0 disables)\r\n"
        "            hh=0..23 mm=0..59 dur=1..255 (default 30)\r\n"
        "  BEEP(freq,vol,ms)\r\n"
        "\r\n"
        "NOTES:\r\n"
        "  Use parentheses and commas in calls.\r\n"
        "\r\n");
}






static void handle_line(char *line)
{
    while (*line == ' ' || *line == '\t') line++;
    if (*line == '\0') return;

    /* normalize CRLF */
    size_t n = strlen(line);
    while (n && (line[n-1] == '\r' || line[n-1] == '\n'))
        line[--n] = '\0';

    if (strcmp(line, "help") == 0)
    {
        print_help();
        return;
    }

    if (strcmp(line, "ping") == 0)
    {
        cdc_write_str("pong\r\n");
        return;
    }

    if (cli_stricmp(line, "mem") == 0)
    {
        uint32_t total = 0, free = 0, min_free = 0, min_tick_ms = 0;
        char min_dt[RTC_DATETIME_STRING_SIZE];
        memset(min_dt, 0, sizeof(min_dt));
        MemMon_Get(&total, &free, &min_free, &min_tick_ms, min_dt, sizeof(min_dt));
        cdc_writef("RAM: total=%luB free=%luB minfree=%luB\r\n",
                   (unsigned long)total, (unsigned long)free, (unsigned long)min_free);
        cdc_writef("RAM: uptime_ms=%lu minfree_at=%s minfree_uptime_ms=%lu\r\n",
                   (unsigned long)HAL_GetTick(),
                   min_dt[0] ? min_dt : "N/A",
                   (unsigned long)min_tick_ms);
        return;
    }

    if (cli_stricmp(line, "findmic") == 0)
    {
        MIC_FindMic(cdc_write_str);
        return;
    }

    if (cli_stricmp(line, "micdiag") == 0)
    {
        MIC_WriteDiag(cdc_write_str);
        return;
    }

    if (strcmp(line, "pascal") == 0 || strcmp(line, "PASCAL") == 0)
    {
        s_pascal_mode = 1;
        mp_start_session();
        return;
    }

    if (cli_is_time0_call(line))
    {
        RTC_WriteTimeYMDHM(cdc_write_str);
        return;
    }

    if (cli_is_call0(line, "micfft"))
    {
        int16_t lf = 0, mf = 0, hf = 0;
        mic_err_t st = MIC_FFT_WaitBinsDbX100(1000u, &lf, &mf, &hf);
        if (st != MIC_ERR_OK)
        {
            const char *msg = MIC_LastErrorMsg();
            cdc_writef("ERR micfft %s(%ld) msg=%s\r\n",
                       MIC_ErrName(st), (long)st, msg ? msg : "");
        }
        else
        {
            cdc_writef("%d,%d,%d\r\n", (int)lf, (int)mf, (int)hf);
        }
        return;
    }

    if (cli_stricmp(line, "charger") == 0)
    {
        CHARGER_WriteStatus(cdc_write_str);
        return;
    }

    if (cli_stricmp(line, "chgrst") == 0)
    {
        CHARGER_Reset();
        cdc_write_str("OK\r\n");
        return;
    }

    if (cli_stricmp(line, "lobatt_enable") == 0)
    {
        CHARGER_LowBattEnableOnce();
        cdc_write_str("OK\r\n");
        return;
    }

    int32_t ret = 0;
    bool has_ret = false;
    if (mp_exec_builtin_line(line, &ret, &has_ret))
    {
        if (has_ret)
        {
            cdc_writef("%ld\r\n", (long)ret);
        }
        else
        {
            cdc_write_str("OK\r\n");
        }
        return;
    }

    dbg_led_blink(1);
    cdc_write_str("ERR unknown, type: help\r\n");
}


void USB_CLI_Init(void)
{
    s_line_len = 0;
    memset(s_line, 0, sizeof(s_line));
    s_usb_connected = 0;
    cdc_write_str("USB CLI ready. Type: help\r\n");
    cdc_prompt();
}

uint8_t USB_CLI_IsConnected(void)
{
    return s_usb_connected;
}

void USB_CLI_Task(void)
{
    uint8_t rx[USB_CLI_RX_CHUNK];
    uint32_t got = 0;
    /* If Pascal mode is active, run Pascal task and route input there */
    if (s_pascal_mode)
    {
        mp_task();  /* Run VM time-slice + abort check */
        
        /* Check if user typed EXIT or session ended */
        if (mp_exit_pending() || !mp_is_active())
        {
            s_pascal_mode = 0;
            mp_stop_session();
            cdc_write_str("\r\nPASCAL EXIT\r\n");
            cdc_prompt();
            return;
        }

         /* Route incoming chars to Pascal */
        uint32_t ret = USBD_CDC_ACM_Receive(rx, sizeof(rx), &got);
        s_usb_connected = (ret == 0) ? 1u : 0u;
        if (ret == 0 && got > 0)
        {
            for (uint32_t i = 0; i < got; i++)
            {
                mp_feed_char((char)rx[i]);
            }
        }
        return;
    }

    uint32_t ret = USBD_CDC_ACM_Receive(rx, sizeof(rx), &got);
    s_usb_connected = (ret == 0) ? 1u : 0u;
    if (ret != 0 || got == 0)
        return;

    for (uint32_t i = 0; i < got; i++)
    {
        char c = (char)rx[i];

        /* Basic line editing + echo */
        if (c == '\b' || c == 0x7F)
        {
            if (s_line_len > 0)
            {
                s_line_len--;
                cdc_write_str("\b \b");
            }
            continue;
        }

        if (c == '\r' || c == '\n')
        {
            /* terminal-friendly newline */
            cdc_write_str("\r\n");

            if (s_line_len > 0)
            {
                s_line[s_line_len] = '\0';
                handle_line(s_line);
                s_line_len = 0;
            }
            cdc_prompt();
            continue;
        }

        /* Echo typed character */
        cdc_echo_char(c);

        if (s_line_len < (USB_CLI_LINE_MAX - 1U))
        {
            s_line[s_line_len++] = c;
        }
        else
        {
            /* overflow -> reset */
            s_line_len = 0;
            dbg_led_blink(2);
            cdc_write_str("\r\nERR line too long\r\n");
            cdc_prompt();
        }
    }
}
