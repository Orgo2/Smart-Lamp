#include "usb_cli.h"

#include "main.h"
#include "ux_device_cdc_acm.h"
#include "led.h"
#include "rtc.h"
#include "analog.h"
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

extern SPI_HandleTypeDef hspi1;

static uint32_t popcount16(uint16_t v)
{
    uint32_t c = 0;
    while (v)
    {
        c += (uint32_t)(v & 1u);
        v >>= 1;
    }
    return c;
}

typedef struct
{
    uint32_t words;
    uint32_t cnt_0000;
    uint32_t cnt_ffff;
    uint32_t transitions;
    uint32_t ones;
    uint16_t minw;
    uint16_t maxw;
    uint16_t first[8];
    uint32_t first_n;
    HAL_StatusTypeDef last_hal;
} micprobe_stats_t;

static void micprobe_pa6_set_input(uint32_t pull)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitTypeDef gi = {0};
    gi.Pin = GPIO_PIN_6;
    gi.Mode = GPIO_MODE_INPUT;
    gi.Pull = pull;
    gi.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &gi);
}

static void micprobe_pa6_set_spi_af(uint32_t pull)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitTypeDef gi = {0};
    gi.Pin = GPIO_PIN_6;
    gi.Mode = GPIO_MODE_AF_PP;
    gi.Pull = pull;
    gi.Speed = GPIO_SPEED_FREQ_HIGH;
    gi.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOA, &gi);
}

static uint8_t micprobe_pa6_read(void)
{
    return (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_6) == GPIO_PIN_SET) ? 1u : 0u;
}

static void micprobe_pa6_pull_diagnose(void)
{
    /* Release SPI pin first (otherwise pull changes may be ignored by AF). */
    micprobe_pa6_set_input(GPIO_NOPULL);
    HAL_Delay(1);
    uint8_t np = micprobe_pa6_read();

    micprobe_pa6_set_input(GPIO_PULLDOWN);
    HAL_Delay(1);
    uint8_t pd = micprobe_pa6_read();

    micprobe_pa6_set_input(GPIO_PULLUP);
    HAL_Delay(1);
    uint8_t pu = micprobe_pa6_read();

    cdc_writef("MICPROBE: PA6(DATA) idle level: NOPULL=%u PULLDOWN=%u PULLUP=%u\r\n",
               (unsigned)np, (unsigned)pd, (unsigned)pu);

    if ((pd == 0u) && (pu == 1u))
        cdc_write_str("MICPROBE: PA6 follows pulls => likely floating/Hi-Z (mic not driving / wrong pin / no power / level mismatch)\r\n");
    else if ((pd == 0u) && (pu == 0u))
        cdc_write_str("MICPROBE: PA6 always LOW => short to GND / mic holding low / logic threshold issue\r\n");
    else if ((pd == 1u) && (pu == 1u))
        cdc_write_str("MICPROBE: PA6 always HIGH => short to VDD / external pull-up too strong\r\n");
}

static void micprobe_stats_reset(micprobe_stats_t *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->minw = 0xFFFFu;
    s->maxw = 0x0000u;
    s->last_hal = HAL_OK;
}

static void micprobe_stats_feed(micprobe_stats_t *s, const uint16_t *buf, uint32_t n, uint16_t *io_prev, uint8_t *io_have_prev)
{
    if (!s || !buf || n == 0) return;
    for (uint32_t i = 0; i < n; i++)
    {
        uint16_t w = buf[i];
        if (s->first_n < (uint32_t)(sizeof(s->first) / sizeof(s->first[0])))
            s->first[s->first_n++] = w;

        if (w == 0x0000u) s->cnt_0000++;
        if (w == 0xFFFFu) s->cnt_ffff++;
        if (w < s->minw) s->minw = w;
        if (w > s->maxw) s->maxw = w;
        s->ones += popcount16(w);

        if (io_have_prev && io_prev)
        {
            if (*io_have_prev)
            {
                if (w != *io_prev) s->transitions++;
            }
            else
            {
                *io_have_prev = 1u;
            }
            *io_prev = w;
        }

        s->words++;
    }
}

static HAL_StatusTypeDef micprobe_rx_words(uint16_t *buf, uint16_t words)
{
    if (!buf || words == 0) return HAL_ERROR;
    return HAL_SPI_Receive(&hspi1, (uint8_t*)buf, words, 500u);
}

static void micprobe_clock_for_ms(uint32_t ms)
{
    uint16_t buf[256];
    uint32_t t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < ms)
    {
        (void)micprobe_rx_words(buf, (uint16_t)(sizeof(buf) / sizeof(buf[0])));
    }
}

static void micprobe_print(const char *tag, uint32_t cpol, uint32_t cpha, const micprobe_stats_t *s)
{
    if (!s) return;

    uint32_t bad = s->cnt_0000 + s->cnt_ffff;
    double ones_pct = (s->words != 0u) ? (100.0 * (double)s->ones / (double)(s->words * 16u)) : 0.0;
    double bad_pct  = (s->words != 0u) ? (100.0 * (double)bad / (double)s->words) : 0.0;

    cdc_writef("%s CPOL=%s CPHA=%s: words=%lu bad=%lu(%.1f%%) ones=%.1f%% trans=%lu min=0x%04X max=0x%04X\r\n",
               tag,
               (cpol == SPI_POLARITY_LOW) ? "LOW" : "HIGH",
               (cpha == SPI_PHASE_1EDGE) ? "1EDGE" : "2EDGE",
               (unsigned long)s->words,
               (unsigned long)bad, bad_pct,
               ones_pct,
               (unsigned long)s->transitions,
               (unsigned int)s->minw,
               (unsigned int)s->maxw);

    cdc_write_str("  first:");
    for (uint32_t i = 0; i < s->first_n; i++)
        cdc_writef(" %04X", (unsigned int)s->first[i]);
    cdc_write_str("\r\n");
}

static void micprobe_run(void)
{
    cdc_write_str("MICPROBE: testing SPI1 edges for PDM data\r\n");
    cdc_write_str("MICPROBE: expected wiring: PA5=CLK(SPI1_SCK), PA6=DATA(SPI1_MISO)\r\n");

    MIC_Stop();
    (void)HAL_SPI_Abort(&hspi1);

    SPI_InitTypeDef saved = hspi1.Init;

    (void)HAL_SPI_DeInit(&hspi1);
    micprobe_pa6_pull_diagnose();

    struct { uint32_t cpol; uint32_t cpha; const char *tag; } modes[] =
    {
        { SPI_POLARITY_LOW,  SPI_PHASE_1EDGE, "mode0" },
        { SPI_POLARITY_LOW,  SPI_PHASE_2EDGE, "mode1" },
        { SPI_POLARITY_HIGH, SPI_PHASE_1EDGE, "mode2" },
        { SPI_POLARITY_HIGH, SPI_PHASE_2EDGE, "mode3" },
    };

    uint32_t best_i = 0xFFFFFFFFu;
    uint32_t best_bad = 0xFFFFFFFFu;
    uint32_t best_trans = 0u;

    for (uint32_t i = 0; i < (uint32_t)(sizeof(modes) / sizeof(modes[0])); i++)
    {
        (void)HAL_SPI_DeInit(&hspi1);
        hspi1.Init = saved;
        hspi1.Init.CLKPolarity = modes[i].cpol;
        hspi1.Init.CLKPhase    = modes[i].cpha;

        if (HAL_SPI_Init(&hspi1) != HAL_OK)
        {
            cdc_writef("%s: HAL_SPI_Init failed\r\n", modes[i].tag);
            continue;
        }

        /* Default CubeMX uses GPIO_PULLDOWN on PA6; keep as baseline. */
        micprobe_pa6_set_spi_af(GPIO_PULLDOWN);

        /* Provide clock for mic wake-up (CMM-4030DT-26154 ~52ms). */
        micprobe_clock_for_ms(MIC_WAKEUP_MS + 10u);

        micprobe_stats_t st;
        micprobe_stats_reset(&st);

        uint16_t buf[256];
        uint16_t prev = 0;
        uint8_t have_prev = 0u;

        const uint32_t total_words = 2048u;
        uint32_t left = total_words;
        while (left)
        {
            uint16_t chunk = (left > (uint32_t)(sizeof(buf) / sizeof(buf[0]))) ? (uint16_t)(sizeof(buf) / sizeof(buf[0])) : (uint16_t)left;
            st.last_hal = micprobe_rx_words(buf, chunk);
            if (st.last_hal != HAL_OK) break;
            micprobe_stats_feed(&st, buf, chunk, &prev, &have_prev);
            left -= chunk;
        }

        micprobe_print(modes[i].tag, modes[i].cpol, modes[i].cpha, &st);

        /* If stuck at 0x0000/0xFFFF, quickly retry with opposite pull to detect floating/open-drain. */
        if ((st.last_hal == HAL_OK) && (st.words != 0u) && ((st.cnt_0000 == st.words) || (st.cnt_ffff == st.words)))
        {
            uint32_t alt_pull = (st.cnt_0000 == st.words) ? GPIO_PULLUP : GPIO_PULLDOWN;
            const char *alt_tag = (alt_pull == GPIO_PULLUP) ? "+PU" : "+PD";
            micprobe_pa6_set_spi_af(alt_pull);

            micprobe_stats_t st2;
            micprobe_stats_reset(&st2);
            uint16_t buf2[256];
            uint16_t prev2 = 0;
            uint8_t have_prev2 = 0u;
            uint32_t left2 = total_words;
            while (left2)
            {
                uint16_t chunk2 = (left2 > (uint32_t)(sizeof(buf2) / sizeof(buf2[0]))) ? (uint16_t)(sizeof(buf2) / sizeof(buf2[0])) : (uint16_t)left2;
                st2.last_hal = micprobe_rx_words(buf2, chunk2);
                if (st2.last_hal != HAL_OK) break;
                micprobe_stats_feed(&st2, buf2, chunk2, &prev2, &have_prev2);
                left2 -= chunk2;
            }

            char tagbuf[12];
            snprintf(tagbuf, sizeof(tagbuf), "%s%s", modes[i].tag, alt_tag);
            micprobe_print(tagbuf, modes[i].cpol, modes[i].cpha, &st2);
        }

        uint32_t bad = st.cnt_0000 + st.cnt_ffff;
        if ((st.last_hal == HAL_OK) && (st.words != 0u))
        {
            if ((bad < best_bad) || ((bad == best_bad) && (st.transitions > best_trans)))
            {
                best_bad = bad;
                best_trans = st.transitions;
                best_i = i;
            }
        }
    }

    if ((best_i != 0xFFFFFFFFu) && (best_bad < 2048u))
    {
        (void)HAL_SPI_DeInit(&hspi1);
        hspi1.Init = saved;
        hspi1.Init.CLKPolarity = modes[best_i].cpol;
        hspi1.Init.CLKPhase    = modes[best_i].cpha;
        if (HAL_SPI_Init(&hspi1) == HAL_OK)
        {
            cdc_writef("MICPROBE: selected %s (apply now). Suggested MX_SPI1_Init: CLKPolarity=%s, CLKPhase=%s\r\n",
                       modes[best_i].tag,
                       (modes[best_i].cpol == SPI_POLARITY_LOW) ? "LOW" : "HIGH",
                       (modes[best_i].cpha == SPI_PHASE_1EDGE) ? "1EDGE" : "2EDGE");
        }
    }
    else
    {
        (void)HAL_SPI_DeInit(&hspi1);
        hspi1.Init = saved;
        (void)HAL_SPI_Init(&hspi1);
        cdc_write_str("MICPROBE: no mode produced non-stuck data. Check wiring/power/LR pin.\r\n");
    }

    MIC_Init();
}

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

static void cli_print_time_ymdhm(void)
{
    char dt[RTC_DATETIME_STRING_SIZE];
    if (RTC_ReadClock(dt) != HAL_OK)
    {
        cdc_write_str("ERR time\r\n");
        return;
    }
    int hh=0,mm=0,ss=0,yy=0,mo=0,dd=0;
    if (sscanf(dt, "%02d:%02d:%02d_%02d.%02d.%02d", &hh,&mm,&ss,&yy,&mo,&dd) != 6)
    {
        cdc_write_str("ERR time\r\n");
        return;
    }
    cdc_writef("%02d,%02d,%02d,%02d,%02d\r\n", yy, mo, dd, hh, mm);
}

static float battery_percent_from_v(float vbat)
{
    /* Simple linear estimate: 3.0V=0%, 4.2V=100%. */
    const float v0 = 3.0f;
    const float v1 = 4.2f;
    if (vbat <= v0) return 0.0f;
    if (vbat >= v1) return 100.0f;
    return (vbat - v0) * 100.0f / (v1 - v0);
}

static const char *charger_state_str(uint8_t st)
{
    switch (st)
    {
        case 1: return "charging";
        case 2: return "charged";
        case 3: return "error";
        default: return "unknown";
    }
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
        "  MICPROBE    (test mic SPI edges)\r\n"
        "  CHARGER()   (battery %, state, VBAT)\r\n"
        "  CHGRST()    (reset charger)\r\n"
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

    if (cli_stricmp(line, "micprobe") == 0)
    {
        micprobe_run();
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
        cli_print_time_ymdhm();
        return;
    }

    if (cli_is_call0(line, "charger"))
    {
        float vbat = ANALOG_GetBat();
        float pct = battery_percent_from_v(vbat);
        uint8_t st = CHARGER_GetStatus();
        cdc_writef("BAT=%.1f%% STATE=%s VBAT=%.2fV\r\n", (double)pct, charger_state_str(st), (double)vbat);
        return;
    }

    if (cli_is_call0(line, "chgrst"))
    {
        CHARGER_Reset();
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
