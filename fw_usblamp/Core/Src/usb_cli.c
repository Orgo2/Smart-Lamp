#include "usb_cli.h"

#include "main.h"
#include "ux_device_cdc_acm.h"
#include "led.h"
#include "bme280.h"
#include "analog.h"
#include "charger.h"
#include "alarm.h"
#include "rtc.h"
#include "mic.h"
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

typedef enum
{
    PENDING_NONE = 0,
    PENDING_BATTERY,
    PENDING_LIGHT,
} pending_cmd_t;

static pending_cmd_t s_pending_cmd;
static uint32_t s_pending_start_id;

/* Pascal interpreter mode */
static uint8_t s_pascal_mode = 0;

static void service_pending_cmd(void)
{
    if (s_pending_cmd == PENDING_NONE)
        return;

    /* Wait for fresh measurement completion (non-blocking). */
    if (ANALOG_IsBusy() != 0U)
        return;

    uint32_t now_id = ANALOG_GetUpdateId();
    if (now_id == s_pending_start_id)
        return;

    if (s_pending_cmd == PENDING_BATTERY)
    {
        uint8_t ctl_cen = CHARGER_IsCharging();
        float v_bat = ANALOG_GetBat();
        uint8_t sta_chg = CHARGER_GetStatus();
        cdc_writef("%u %.3f %u\r\n", ctl_cen, (double)v_bat, sta_chg);
        s_pending_cmd = PENDING_NONE;
        cdc_prompt();
        return;
    }

    if (s_pending_cmd == PENDING_LIGHT)
    {
        float lux = ANALOG_GetLight();
        cdc_writef("%.2f\r\n", (double)lux);
        s_pending_cmd = PENDING_NONE;
        cdc_prompt();
        return;
    }
}

static void print_help(void)
{
    cdc_write_str(
        "PRIKAZY:\r\n"
        "  HELP\r\n"
        "  PING\r\n"
        "  PASCAL                        (spusti Pascal interpreter)\r\n"
        "  LEDSTATUS\r\n"
        "  LEDOFF\r\n"
        "  LEDON [R G B W]\r\n"
        "  LED <1-30> <R> <G> <B> <W>    (0..255)\r\n"
        "  TEMPERATURE\r\n"
        "  PRESSURE\r\n"
        "  HUMIDITY\r\n"
        "  BME280\r\n"
        "  AUDIO\r\n"
        "  BATTERY\r\n"
        "  LIGHT\r\n"
        "  BUTTON                        (vypise stav tlacidiel B1 B2 BL)\r\n"
        "  BEEP <FREQ> <VOLUME> <CAS>\r\n"
        "  RTC                           (vypise YY MM DD HH MM)\r\n"
        "  RTC <YY> <MM> <DD> <HH> <MM>  (nastavi datum a cas)\r\n"
        "  ALARM                         (vypise HH MM TRVANIE TRIGGER)\r\n"
        "  ALARM <HH> <MM> <TRVANIE>     (nastavi denný alarm)\r\n"
        "  RNG                           (nahodne cislo 0-255)\r\n"
        "\r\n"
        "POZNAMKY:\r\n"
        "  LEDON bez parametrov = 100 100 100 100\r\n"
        "  LEDON R G B W nastavi vsetky LED\r\n"
        "  AUDIO: hlasitost v dBFS (PDM mikrofon)\r\n"
        "  BATTERY: CTL_CEN (0=nabija,1=nenabija), V, STA_CHG (1=nabija,2=nenabija,3=err)\r\n"
        "  BATTERY RST: reset nabijacky (toggle CTL_CEN)\r\n"
        "  LIGHT: intenzita svetla (lux)\r\n"
        "  BEEP: FREQ Hz, VOLUME 0-50 (50=max), CAS v sekundach\r\n"
        "  RTC: sekundy sa cez CLI nenastavuju\r\n"
        "  ALARM: zvoni kazdy den v HH:MM, TRVANIE v sekundach, TRIGGER=1 ked zvoni\r\n"
        "  RNG: generuje 8-bitove nahodne cislo (0-255)\r\n"
        "\r\n"
        "PRIKLADY:\r\n"
        "  LEDON 10 0 0 0\r\n"
        "  LED 1 254 0 0 0\r\n"
        "  LEDOFF\r\n"
        "  BEEP 1000 50 0.5\r\n"
        "  RTC 25 12 24 08 30\r\n"
        "  ALARM 07 00 30\r\n"
        "  BATTERY RST\r\n"
        "\r\n");
}

static int parse_u8(const char *s, uint16_t max, uint8_t *out)
{
    if (!s || !out) return 0;
    char *endp = NULL;
    long v = strtol(s, &endp, 10);
    if (endp == s || *endp != '\0') return 0;
    if (v < 0 || v > (long)max) return 0;
    *out = (uint8_t)v;
    return 1;
}

static int parse_f32(const char *s, float min_v, float max_v, float *out)
{
    if (!s || !out) return 0;
    char *endp = NULL;
    float v = strtof(s, &endp);
    if (endp == s || *endp != '\0') return 0;
    if (v < min_v || v > max_v) return 0;
    *out = v;
    return 1;
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

   ///////////////ovladanie digitalnych LED diod////////////////
    if (strcmp(line, "ledoff") == 0)
    {
        led_set_all_RGBW(0, 0, 0, 0);
        led_render();
        cdc_write_str("OK ledoff\r\n");
        return;
    }

    /* ledon [r g b w] */
    if (strncmp(line, "ledon", 5) == 0)
    {
        /* default: ledon */
        if (strcmp(line, "ledon") == 0)
        {
            led_set_all_RGBW(100, 100, 100, 100);
            led_render();
            cdc_write_str("OK ledon 100 100 100 100\r\n");
            return;
        }

        /* ledon <r> <g> <b> <w> */
        if (strncmp(line, "ledon ", 6) == 0)
        {
            char *tok = strtok(line, " "); /* ledon */
            (void)tok;
            char *s_r = strtok(NULL, " ");
            char *s_g = strtok(NULL, " ");
            char *s_b = strtok(NULL, " ");
            char *s_w = strtok(NULL, " ");

            uint8_t r, g, b, w;
            if (!parse_u8(s_r, 255, &r) ||
                !parse_u8(s_g, 255, &g) ||
                !parse_u8(s_b, 255, &b) ||
                !parse_u8(s_w, 255, &w))
            {
                dbg_led_blink(1);
                cdc_write_str("ERR usage: ledon <r> <g> <b> <w> (0..255)\r\n");
                return;
            }

            led_set_all_RGBW(r, g, b, w);
            led_render();
            cdc_write_str("OK\r\n");
            return;
        }

        dbg_led_blink(1);
        cdc_write_str("ERR usage: ledon [r g b w]\r\n");
        return;
    }

    /* led <n> <r> <g> <b> <w> */
    if (strncmp(line, "led ", 4) == 0)
    {
        char *tok = strtok(line, " "); /* led */
        (void)tok;
        char *s_idx = strtok(NULL, " ");
        char *s_r   = strtok(NULL, " ");
        char *s_g   = strtok(NULL, " ");
        char *s_b   = strtok(NULL, " ");
        char *s_w   = strtok(NULL, " ");

        uint8_t idx, r, g, b, w;
        if (!parse_u8(s_idx, 30, &idx) || idx == 0 ||
            !parse_u8(s_r, 255, &r) ||
            !parse_u8(s_g, 255, &g) ||
            !parse_u8(s_b, 255, &b) ||
            !parse_u8(s_w, 255, &w))
        {
            dbg_led_blink(1);
            cdc_write_str("ERR usage: led <1-30> <r> <g> <b> <w> (0..255)\r\n");
            return;
        }

        /* Convert from 1-based to 0-based index */
        led_set_RGBW(idx - 1, r, g, b, w);
        led_render();
        cdc_write_str("OK\r\n");
        return;
    }
    ///////////////////////////tlakomer BME280///////////////////////////////
       if (strcmp(line, "temperature") == 0)
       {
    	   float t = 0.0f;
           if (T(&t) == HAL_OK) cdc_writef("%.2f\r\n", (double)t);
           else { dbg_led_blink(1); cdc_write_str("ERR temperature\r\n"); }
           return;
       }

       if (strcmp(line, "pressure") == 0)
       {
    	   float p = 0.0f;
           if (P(&p) == HAL_OK) cdc_writef("%.2f\r\n", (double)p);  // driver vraj vracia hPa
           else { dbg_led_blink(1); cdc_write_str("ERR pressure\r\n"); }
           return;
       }

       if (strcmp(line, "humidity") == 0)
       {
    	   float h = 0.0f;
           if (RH(&h) == HAL_OK) cdc_writef("%.2f\r\n", (double)h);
           else { dbg_led_blink(1); cdc_write_str("ERR humidity\r\n"); }
           return;
       }

       if (strcmp(line, "bme280") == 0)
       {
    	   BME280_Data_t d;
           if (BME280(&d) == HAL_OK)
           {
               cdc_writef("T=%.2fC P=%.2fhPa RH=%.2f%%\r\n",
                          (double)d.temperature, (double)d.pressure, (double)d.humidity);
           }
           else
           {
               dbg_led_blink(1);
               cdc_write_str("ERR bme280\r\n");
           }
           return;
       }
    
    if (strcmp(line, "audio") == 0)
    {
        float dbfs = 0.0f;
        float rms  = 0.0f;
        mic_err_t e = MIC_GetLast50ms(&dbfs, &rms);

        if (e != MIC_ERR_OK)
        {
            /*
             * Podľa zadania: ak je detekovaný niektorý chybový stav,
             * vypíš debug správu NAMIETO dát.
             */
            switch (e)
            {
                case MIC_ERR_NOT_INIT:
                    cdc_write_str("ERR mic: not init\r\n");
                    break;
                case MIC_ERR_SPI_NOT_READY:
                    cdc_write_str("ERR mic: SPI not READY (busy)\r\n");
                    break;
                case MIC_ERR_START_DMA:
                    cdc_write_str("ERR mic: failed to start SPI DMA\r\n");
                    break;
                case MIC_ERR_TIMEOUT:
                    cdc_write_str("ERR mic: timeout / measuring... try again\r\n");
                    break;
                case MIC_ERR_SPI_ERROR:
                    cdc_write_str("ERR mic: SPI error\r\n");
                    break;
                case MIC_ERR_DMA_NO_WRITE:
                    cdc_write_str("ERR mic: DMA complete but no data written (buffer unchanged)\r\n");
                    break;
                case MIC_ERR_DATA_STUCK:
                    cdc_write_str("ERR mic: PDM DATA stuck (constant / no transitions)\r\n");
                    break;
                case MIC_ERR_SIGNAL_SATURATED:
                    cdc_write_str("ERR mic: signal saturated (RMS/PEAK ~ 1.0)\r\n");
                    break;
                default:
                    cdc_write_str("ERR mic: unknown\r\n");
                    break;
            }

            /*
             * Automatický dump DMA: len pri typických chybách "0x0000 / 0xFFFF / stuck".
             * Toto nahrádza samostatný príkaz audiodump.
             */
            if (e == MIC_ERR_DATA_STUCK || e == MIC_ERR_SIGNAL_SATURATED)
            {
                uint32_t words = 0;
                const uint16_t *p = MIC_DebugLastDmaBuf(&words);
                if (p != NULL && words > 0)
                {
                    uint32_t nwords = (words < 16u) ? words : 16u;
                    cdc_writef("DMA dump first %lu words:\r\n", (unsigned long)nwords);
                    for (uint32_t i = 0; i < nwords; i++)
                    {
                        cdc_writef("%04X%s", (unsigned int)p[i], ((i % 16u) == 15u || i == (nwords - 1u)) ? "\r\n" : " ");
                    }
                }
            }
            return;
        }

        /* OK data */
        cdc_writef("rms=%.4f dbfs=%.2f\r\n", (double)rms, (double)dbfs);
        return;
    }
    
    if (strcmp(line, "battery rst") == 0)
    {
        CHARGER_Reset();
        cdc_write_str("OK\r\n");
        return;
    }
    
    if (strcmp(line, "battery") == 0)
    {
        if (s_pending_cmd != PENDING_NONE)
        {
            cdc_write_str("BUSY\r\n");
            return;
        }

        s_pending_cmd = PENDING_BATTERY;
        s_pending_start_id = ANALOG_GetUpdateId();
        ANALOG_RequestUpdate();
        return;
    }
    
    if (strcmp(line, "light") == 0)
    {
        if (s_pending_cmd != PENDING_NONE)
        {
            cdc_write_str("BUSY\r\n");
            return;
        }

        s_pending_cmd = PENDING_LIGHT;
        s_pending_start_id = ANALOG_GetUpdateId();
        ANALOG_RequestUpdate();
        return;
    }

    if (strcmp(line, "button") == 0)
    {
        /* Prečítaj stav tlačidiel B1, B2, BL (sú konfigurované v main.h) */
        uint8_t b1 = (uint8_t)HAL_GPIO_ReadPin(B1_GPIO_Port, B1_Pin);
        uint8_t b2 = (uint8_t)HAL_GPIO_ReadPin(B2_GPIO_Port, B2_Pin);
        uint8_t bl = (uint8_t)HAL_GPIO_ReadPin(BL_GPIO_Port, BL_Pin);
        
        /* Vypíš ako trojmiestne číslo: B1B2BL (každý je 0 alebo 1) */
        cdc_writef("%u%u%u\r\n", (unsigned int)b1, (unsigned int)b2, (unsigned int)bl);
        return;
    }

    /* rtc : read or set date/time
     * Format: rtc <YY> <MM> <DD> <HH> <MM>
     * Read prints the same 5 fields (YY MM DD HH MM)
     */
    if (strncmp(line, "rtc", 3) == 0 || strncmp(line, "RTC", 3) == 0)
    {
        if (strcmp(line, "rtc") == 0 || strcmp(line, "RTC") == 0)
        {
            char dt[RTC_DATETIME_STRING_SIZE];
            if (RTC_ReadClock(dt) != HAL_OK)
            {
                dbg_led_blink(1);
                cdc_write_str("ERR rtc read\r\n");
                return;
            }

            int hh, mm, ss, yy, mo, dd;
            if (sscanf(dt, "%02d:%02d:%02d_%02d.%02d.%02d", &hh, &mm, &ss, &yy, &mo, &dd) != 6)
            {
                dbg_led_blink(1);
                cdc_write_str("ERR rtc parse\r\n");
                return;
            }

            cdc_writef("%02d %02d %02d %02d %02d\r\n", yy, mo, dd, hh, mm);
            return;
        }

        char *tok = strtok(line, " "); /* rtc */
        (void)tok;
        char *s_yy = strtok(NULL, " ");
        char *s_mo = strtok(NULL, " ");
        char *s_dd = strtok(NULL, " ");
        char *s_hh = strtok(NULL, " ");
        char *s_mm = strtok(NULL, " ");

        uint8_t yy, mo, dd, hh, mm;
        if (!parse_u8(s_yy, 99, &yy) ||
            !parse_u8(s_mo, 12, &mo) || mo == 0 ||
            !parse_u8(s_dd, 31, &dd) || dd == 0 ||
            !parse_u8(s_hh, 23, &hh) ||
            !parse_u8(s_mm, 59, &mm))
        {
            dbg_led_blink(1);
            cdc_write_str("ERR usage: rtc <YY> <MM> <DD> <HH> <MM>\r\n");
            return;
        }

        char buf[RTC_DATETIME_STRING_SIZE];
        /* seconds set to 00 */
        snprintf(buf, sizeof(buf), "%02u:%02u:00_%02u.%02u.%02u", hh, mm, yy, mo, dd);

        if (RTC_SetClock(buf) != HAL_OK)
        {
            dbg_led_blink(1);
            cdc_write_str("ERR rtc set\r\n");
            return;
        }

        cdc_write_str("OK\r\n");
        return;
    }

    /* BEEP <freq> <volume0-50> <time_s> */
    if (strncmp(line, "BEEP", 4) == 0 || strncmp(line, "beep", 4) == 0)
    {
        if (strcmp(line, "BEEP") == 0 || strcmp(line, "beep") == 0)
        {
            dbg_led_blink(1);
            cdc_write_str("ERR usage: BEEP <freq_hz> <volume0-50> <time_s>\r\n");
            return;
        }

        char *tok = strtok(line, " "); /* BEEP */
        (void)tok;
        char *s_f = strtok(NULL, " ");
        char *s_v = strtok(NULL, " ");
        char *s_t = strtok(NULL, " ");

        uint8_t volume = 0;
        float time_s = 0.0f;
        char *endp = NULL;
        unsigned long freq_ul = strtoul(s_f ? s_f : "", &endp, 10);
        if (endp == (s_f ? s_f : "") || (endp && *endp != '\0') || freq_ul == 0 || freq_ul > 50000UL ||
            !parse_u8(s_v, 50, &volume) ||
            !parse_f32(s_t, 0.0f, 3600.0f, &time_s))
        {
            dbg_led_blink(1);
            cdc_write_str("ERR usage: BEEP <freq_hz> <volume0-50> <time_s>\r\n");
            return;
        }

        BEEP((uint16_t)freq_ul, volume, time_s);
        cdc_write_str("OK\r\n");
        return;
    }

    /* alarm <HH> <MM> <duration_s> ; alarm -> read current */
    if (strncmp(line, "alarm", 5) == 0 || strncmp(line, "ALARM", 5) == 0)
    {
        /* read */
        if (strcmp(line, "alarm") == 0 || strcmp(line, "ALARM") == 0)
        {
            uint8_t hh, mm, dur;
            if (RTC_GetDailyAlarm(&hh, &mm, &dur) != HAL_OK)
            {
                dbg_led_blink(1);
                cdc_write_str("ERR alarm not set\r\n");
                return;
            }
            cdc_writef("%02u %02u %u %u\r\n", hh, mm, dur, (unsigned int)RTC_AlarmTrigger);
            return;
        }

        char *tok = strtok(line, " "); /* alarm */
        (void)tok;
        char *s_hh = strtok(NULL, " ");
        char *s_mm = strtok(NULL, " ");
        char *s_dur = strtok(NULL, " ");

        uint8_t hh, mm;
        uint8_t dur_u8;
        char *endp = NULL;
        if (!parse_u8(s_hh, 23, &hh) ||
            !parse_u8(s_mm, 59, &mm) ||
            s_dur == NULL)
        {
            dbg_led_blink(1);
            cdc_write_str("ERR usage: alarm <HH> <MM> <duration_s>\r\n");
            return;
        }

        unsigned long dur_ul = strtoul(s_dur, &endp, 10);
        if (endp == s_dur || (endp && *endp != '\0') || dur_ul > 255UL)
        {
            dbg_led_blink(1);
            cdc_write_str("ERR usage: alarm <HH> <MM> <duration_s>\r\n");
            return;
        }
        dur_u8 = (uint8_t)dur_ul;

        if (RTC_SetDailyAlarm(hh, mm, dur_u8) != HAL_OK)
        {
            dbg_led_blink(1);
            cdc_write_str("ERR alarm set\r\n");
            return;
        }

        cdc_write_str("OK\r\n");
        return;
    }

    /* rng - generate random 8-bit number */
    if (strcmp(line, "rng") == 0 || strcmp(line, "RNG") == 0)
    {
        extern RNG_HandleTypeDef hrng;
        uint32_t random32 = 0;
        
        if (HAL_RNG_GenerateRandomNumber(&hrng, &random32) == HAL_OK)
        {
            uint8_t random8 = (uint8_t)(random32 & 0xFF);
            cdc_writef("%u\r\n", (unsigned int)random8);
        }
        else
        {
            cdc_write_str("ERR rng\r\n");
        }
        return;
    }

    /* pascal - enter Pascal interpreter mode */
    if (strcmp(line, "pascal") == 0 || strcmp(line, "PASCAL") == 0)
    {
        s_pascal_mode = 1;
        mp_start_session();
        return;
    }
    
    dbg_led_blink(1);
    cdc_write_str("ERR unknown, type: help\r\n");


}

void USB_CLI_Init(void)
{
    s_line_len = 0;
    memset(s_line, 0, sizeof(s_line));

    s_pending_cmd = PENDING_NONE;
    s_pending_start_id = 0;

    cdc_write_str("USB CLI ready. Type: help\r\n");
    cdc_prompt();
}

void USB_CLI_Task(void)
{
    uint8_t rx[USB_CLI_RX_CHUNK];
    uint32_t got = 0;

    /* Allow async replies for single-shot measurements without blocking USB. */
    service_pending_cmd();

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
