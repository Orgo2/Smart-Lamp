#include "usb_cli.h"

#include "main.h"
#include "ux_device_cdc_acm.h"
#include "led.h"
#include <MiniPascal.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdint.h>

#ifndef USB_CLI_RX_CHUNK
#define USB_CLI_RX_CHUNK 64
#endif

#ifndef USB_CLI_LINE_MAX
#define USB_CLI_LINE_MAX 128
#endif

static char s_line[USB_CLI_LINE_MAX];
static uint32_t s_line_len;

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
        HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET);
        HAL_Delay(on_ms);
        HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);
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

static void print_help(void)
{
    cdc_write_str(
        "COMMANDS:\r\n"
        "  HELP\r\n"
        "  PING\r\n"
        "  PASCAL      (enter interpreter)\r\n"
        "  LEDSTATUS\r\n"
        "\r\n"
        "PASCAL CALLS (same as interpreter):\r\n"
        "  LED(i,r,g,b,w)\r\n"
        "  LEDON(r,g,b,w)\r\n"
        "  LEDOFF()\r\n"
        "  WAIT(ms) / DELAY(ms)\r\n"
        "  BATTERY()\r\n"
        "  LIGHT()\r\n"
        "  BTN()\r\n"
        "  RNG()\r\n"
        "  TEMP()\r\n"
        "  HUM()\r\n"
        "  PRESS()\r\n"
        "  MIC()\r\n"
        "  SETTIME(h,m,s)\r\n"
        "  ALARM() / ALARM(h,m,s)\r\n"
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

    if (strcmp(line, "ledstatus") == 0)
    {
        char buf[80];
        led_hw_status(buf, sizeof(buf));
        cdc_write_str(buf);
        cdc_write_str("\r\n");
        return;
    }

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

    if (strcmp(line, "pascal") == 0 || strcmp(line, "PASCAL") == 0)
    {
        s_pascal_mode = 1;
        mp_start_session();
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
    cdc_write_str("USB CLI ready. Type: help\r\n");
    cdc_prompt();
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
        if (USBD_CDC_ACM_Receive(rx, sizeof(rx), &got) == 0 && got > 0)
        {
            for (uint32_t i = 0; i < got; i++)
            {
                mp_feed_char((char)rx[i]);
            }
        }
        return;
    }

    if (USBD_CDC_ACM_Receive(rx, sizeof(rx), &got) != 0 || got == 0)
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
