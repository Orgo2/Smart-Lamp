/*
 * rtc.c - RTC helpers (time/date + alarm trigger flag).
 * Uses CubeMX-initialized HAL RTC handle (hrtc).
 */

#include "rtc.h"
#include "main.h"
#include <stdio.h>
#include <string.h>

/* External RTC handle from main.c (initialized by MX_RTC_Init). */
extern RTC_HandleTypeDef hrtc;

/* Alarm duration tracking (trigger flag only). */
static uint8_t alarm_duration_sec = 0;
static uint8_t alarm_elapsed_sec = 0;
static uint8_t alarm_active = 0;

/* Public trigger flag: 1 when alarm is active/ringing */
volatile uint8_t RTC_AlarmTrigger = 0;

/* Store configured daily alarm time (HH:MM) and duration */
static uint8_t alarm_cfg_hh = 0;
static uint8_t alarm_cfg_mm = 0;
static uint8_t alarm_cfg_duration = 0;
static volatile uint8_t alarm_cfg_valid = 0;

HAL_StatusTypeDef RTC_Init(void)
{
    /* RTC is initialized by CubeMX (MX_RTC_Init); keep this for API compatibility. */
    return HAL_OK;
}

HAL_StatusTypeDef RTC_ReadClock(char *datetime_str)
{
    RTC_TimeTypeDef sTime = {0};
    RTC_DateTypeDef sDate = {0};
    
    if (datetime_str == NULL)
    {
        return HAL_ERROR;
    }
    
    /* Read time then date to unlock shadow registers. */
    if (HAL_RTC_GetTime(&hrtc, &sTime, RTC_FORMAT_BIN) != HAL_OK)
    {
        return HAL_ERROR;
    }
    
    if (HAL_RTC_GetDate(&hrtc, &sDate, RTC_FORMAT_BIN) != HAL_OK)
    {
        return HAL_ERROR;
    }
    
    /* Format: "HH:MM:SS_YY.MM.DD". */
    snprintf(datetime_str, RTC_DATETIME_STRING_SIZE,
             "%02d:%02d:%02d_%02d.%02d.%02d",
             sTime.Hours,
             sTime.Minutes,
             sTime.Seconds,
             sDate.Year,
             sDate.Month,
             sDate.Date);
    
    return HAL_OK;
}

HAL_StatusTypeDef RTC_GetYMDHMS(int *yy, int *mo, int *dd, int *hh, int *mm, int *ss)
{
    if (!yy || !mo || !dd || !hh || !mm || !ss) return HAL_ERROR;

    char dt[RTC_DATETIME_STRING_SIZE];
    if (RTC_ReadClock(dt) != HAL_OK) return HAL_ERROR;

    int t_h=0,t_m=0,t_s=0,t_y=0,t_mo=0,t_d=0;
    if (sscanf(dt, "%02d:%02d:%02d_%02d.%02d.%02d", &t_h,&t_m,&t_s,&t_y,&t_mo,&t_d) != 6) return HAL_ERROR;

    *yy = t_y;
    *mo = t_mo;
    *dd = t_d;
    *hh = t_h;
    *mm = t_m;
    *ss = t_s;
    return HAL_OK;
}

HAL_StatusTypeDef RTC_SetClock(const char *datetime_str)
{
    RTC_TimeTypeDef sTime = {0};
    RTC_DateTypeDef sDate = {0};
    int hours, minutes, seconds, year, month, day;
    
    if (datetime_str == NULL)
    {
        return HAL_ERROR;
    }
    
    /* Parse "HH:MM:SS_YY.MM.DD". */
    if (sscanf(datetime_str, "%02d:%02d:%02d_%02d.%02d.%02d",
               &hours, &minutes, &seconds, &year, &month, &day) != 6)
    {
        return HAL_ERROR;
    }
    
    /* Validate ranges. */
    if (hours > 23 || minutes > 59 || seconds > 59 ||
        year > 99 || month < 1 || month > 12 || day < 1 || day > 31)
    {
        return HAL_ERROR;
    }
    
    /* Set time. */
    sTime.Hours = hours;
    sTime.Minutes = minutes;
    sTime.Seconds = seconds;
    sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    sTime.StoreOperation = RTC_STOREOPERATION_RESET;
    
    if (HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN) != HAL_OK)
    {
        return HAL_ERROR;
    }
    
    /* Set date. */
    sDate.Year = year;
    sDate.Month = month;
    sDate.Date = day;
    sDate.WeekDay = RTC_WEEKDAY_MONDAY;
    
    if (HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN) != HAL_OK)
    {
        return HAL_ERROR;
    }
    
    return HAL_OK;
}

void RTC_WriteTimeYMDHM(rtc_write_fn_t write)
{
    if (!write) return;

    int yy=0,mo=0,dd=0,hh=0,mm=0,ss=0;
    if (RTC_GetYMDHMS(&yy, &mo, &dd, &hh, &mm, &ss) != HAL_OK)
    {
        write("ERR time\r\n");
        return;
    }

    char buf[32];
    snprintf(buf, sizeof(buf), "%02d,%02d,%02d,%02d,%02d\r\n", yy, mo, dd, hh, mm);
    write(buf);
}

HAL_StatusTypeDef RTC_SetAlarm(const char *alarm_str, uint8_t duration_sec, uint8_t callback_interval_sec)
{
    RTC_AlarmTypeDef sAlarm = {0};
    int hours, minutes, seconds;
    
    (void)callback_interval_sec; /* Not used - callbacks removed, only trigger flag */
    
    if (alarm_str == NULL)
    {
        return HAL_ERROR;
    }
    
    /* Disable alarm if alarm_str is "0" or duration is 0. */
    if ((alarm_str[0] == '0' && alarm_str[1] == '\0') || duration_sec == 0)
    {
        alarm_active = 0;
        alarm_duration_sec = 0;
        alarm_elapsed_sec = 0;
        RTC_AlarmTrigger = 0;
        alarm_cfg_valid = 0;
        
        if (HAL_RTC_DeactivateAlarm(&hrtc, RTC_ALARM_A) != HAL_OK)
        {
            return HAL_ERROR;
        }
        return HAL_OK;
    }
    
    /* Parse "HH:MM:SS". */
    if (sscanf(alarm_str, "%02d:%02d:%02d", &hours, &minutes, &seconds) != 3)
    {
        return HAL_ERROR;
    }
    
    /* Validate ranges. */
    if (hours > 23 || minutes > 59 || seconds > 59)
    {
        return HAL_ERROR;
    }
    
    /* Store duration and clear trigger state. */
    alarm_duration_sec = duration_sec;
    alarm_elapsed_sec = 0;
    alarm_active = 0;
    RTC_AlarmTrigger = 0;
    
    /* Configure alarm A. */
    sAlarm.AlarmTime.Hours = hours;
    sAlarm.AlarmTime.Minutes = minutes;
    sAlarm.AlarmTime.Seconds = seconds;
    sAlarm.AlarmTime.SubSeconds = 0;
    sAlarm.AlarmTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
    sAlarm.AlarmTime.StoreOperation = RTC_STOREOPERATION_RESET;
    sAlarm.AlarmMask = RTC_ALARMMASK_DATEWEEKDAY;
    sAlarm.AlarmSubSecondMask = RTC_ALARMSUBSECONDMASK_ALL;
    sAlarm.AlarmDateWeekDaySel = RTC_ALARMDATEWEEKDAYSEL_DATE;
    sAlarm.AlarmDateWeekDay = 1;
    sAlarm.Alarm = RTC_ALARM_A;
    
    if (HAL_RTC_SetAlarm_IT(&hrtc, &sAlarm, RTC_FORMAT_BIN) != HAL_OK)
    {
        return HAL_ERROR;
    }
    
    /* Enable EXTI line 28 for RTC Alarm interrupt - required on STM32U0 */
    __HAL_RTC_ALARM_EXTI_ENABLE_IT();
    
    return HAL_OK;
}

HAL_StatusTypeDef RTC_SetDailyAlarm(uint8_t hh, uint8_t mm, uint8_t duration_sec)
{
    if (duration_sec == 0)
    {
        return RTC_SetAlarm("0", 0, 0);
    }

    if (hh > 23 || mm > 59 || duration_sec > 255)
    {
        return HAL_ERROR;
    }

    /* Cache config immediately so CLI can read back even before next IRQ */
    alarm_cfg_hh = hh;
    alarm_cfg_mm = mm;
    alarm_cfg_duration = duration_sec;
    alarm_cfg_valid = 1;
    RTC_AlarmTrigger = 0;

    /* Determine next trigger second based on current time */
    RTC_TimeTypeDef now = {0};
    RTC_DateTypeDef nowDate = {0};
    HAL_RTC_GetTime(&hrtc, &now, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&hrtc, &nowDate, RTC_FORMAT_BIN); /* MUST read date to unlock RTC shadow registers! */

    uint8_t ah = hh;
    uint8_t am = mm;
    uint8_t as = 0; /* default to :00 */

    if (now.Hours == hh && now.Minutes == mm)
    {
        /* We are in the target minute already; trigger on next second */
        if (now.Seconds >= 59)
        {
            /* roll to next minute */
            as = 0;
            am = (mm + 1) % 60;
            if (am == 0)
            {
                ah = (hh + 1) % 24;
            }
        }
        else
        {
            as = now.Seconds + 1;
        }
    }
    else
    {
        /* If current time already past target, leave as hh:mm:00 (will fire next day) */
        /* If current time before target, keep hh:mm:00 today */
        as = 0;
    }

    char buf[RTC_ALARM_STRING_SIZE];
    snprintf(buf, sizeof(buf), "%02u:%02u:%02u", ah, am, as);
    HAL_StatusTypeDef st = RTC_SetAlarm(buf, duration_sec, 1); /* callback_interval ignored */
    if (st != HAL_OK)
    {
        /* rollback cache on failure */
        alarm_cfg_valid = 0;
        alarm_cfg_duration = 0;
    }
    return st;
}

HAL_StatusTypeDef RTC_GetDailyAlarm(uint8_t *hh, uint8_t *mm, uint8_t *duration_sec)
{
    if (hh == NULL || mm == NULL || duration_sec == NULL)
    {
        return HAL_ERROR;
    }

    /* If never configured, return zeros (midnight, duration 0) */
    if (alarm_cfg_duration == 0 && alarm_cfg_valid == 0)
    {
        *hh = 0;
        *mm = 0;
        *duration_sec = 0;
    }
    else
    {
        *hh = alarm_cfg_hh;
        *mm = alarm_cfg_mm;
        *duration_sec = alarm_cfg_duration;
    }
    return HAL_OK;
}



void HAL_RTC_AlarmAEventCallback(RTC_HandleTypeDef *hrtc_ptr)
{
    if (!alarm_active)
    {
        /* First alarm trigger - start duration counting */
        alarm_active = 1;
        alarm_elapsed_sec = 0;
        RTC_AlarmTrigger = 1;
        
        RTC_TimeTypeDef sTime = {0};
        RTC_AlarmTypeDef sAlarm = {0};
        
        HAL_RTC_GetTime(hrtc_ptr, &sTime, RTC_FORMAT_BIN);
        
        /* Schedule next tick +1s with proper carry to minutes/hours */
        uint8_t nsec = sTime.Seconds + 1;
        uint8_t nmin = sTime.Minutes;
        uint8_t nhrs = sTime.Hours;
        if (nsec >= 60)
        {
            nsec = 0;
            nmin++;
            if (nmin >= 60)
            {
                nmin = 0;
                nhrs = (nhrs + 1) % 24;
            }
        }

        sAlarm.AlarmTime.Hours = nhrs;
        sAlarm.AlarmTime.Minutes = nmin;
        sAlarm.AlarmTime.Seconds = nsec;
        sAlarm.AlarmTime.SubSeconds = 0;
        sAlarm.AlarmTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
        sAlarm.AlarmTime.StoreOperation = RTC_STOREOPERATION_RESET;
        sAlarm.AlarmMask = RTC_ALARMMASK_DATEWEEKDAY;
        sAlarm.AlarmSubSecondMask = RTC_ALARMSUBSECONDMASK_ALL;
        sAlarm.AlarmDateWeekDaySel = RTC_ALARMDATEWEEKDAYSEL_DATE;
        sAlarm.AlarmDateWeekDay = 1;
        sAlarm.Alarm = RTC_ALARM_A;
        
        HAL_RTC_SetAlarm_IT(hrtc_ptr, &sAlarm, RTC_FORMAT_BIN);
    }
    else
    {
        /* Duration counting - increment elapsed time */
        alarm_elapsed_sec++;
        
        if (alarm_elapsed_sec >= alarm_duration_sec)
        {
            /* Duration expired - reset trigger and deactivate */
            alarm_active = 0;
            alarm_elapsed_sec = 0;
            RTC_AlarmTrigger = 0;
            HAL_RTC_DeactivateAlarm(hrtc_ptr, RTC_ALARM_A);
        }
        else
        {
            /* Reschedule for next second with proper carry */
            RTC_TimeTypeDef sTime = {0};
            RTC_AlarmTypeDef sAlarm = {0};
            
            HAL_RTC_GetTime(hrtc_ptr, &sTime, RTC_FORMAT_BIN);
            
            uint8_t nsec = sTime.Seconds + 1;
            uint8_t nmin = sTime.Minutes;
            uint8_t nhrs = sTime.Hours;
            if (nsec >= 60)
            {
                nsec = 0;
                nmin++;
                if (nmin >= 60)
                {
                    nmin = 0;
                    nhrs = (nhrs + 1) % 24;
                }
            }
            
            sAlarm.AlarmTime.Hours = nhrs;
            sAlarm.AlarmTime.Minutes = nmin;
            sAlarm.AlarmTime.Seconds = nsec;
            sAlarm.AlarmTime.SubSeconds = 0;
            sAlarm.AlarmTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
            sAlarm.AlarmTime.StoreOperation = RTC_STOREOPERATION_RESET;
            sAlarm.AlarmMask = RTC_ALARMMASK_DATEWEEKDAY;
            sAlarm.AlarmSubSecondMask = RTC_ALARMSUBSECONDMASK_ALL;
            sAlarm.AlarmDateWeekDaySel = RTC_ALARMDATEWEEKDAYSEL_DATE;
            sAlarm.AlarmDateWeekDay = 1;
            sAlarm.Alarm = RTC_ALARM_A;
            
            HAL_RTC_SetAlarm_IT(hrtc_ptr, &sAlarm, RTC_FORMAT_BIN);
        }
    }
}
