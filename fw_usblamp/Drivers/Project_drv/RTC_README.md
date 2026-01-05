# RTC Driver - Real Time Clock

## Popis
Driver pre STM32U0 s externým LSE krištáľom (32.768 kHz) pre presné meranie času.

## API Funkcie

### 1. RTC_Init()
```c
HAL_StatusTypeDef RTC_Init(void);
```
Inicializuje RTC s externým LSE krištáľom. Volať raz pri štarte programu.

**Návratová hodnota:** 
- `HAL_OK` - úspešná inicializácia
- `HAL_ERROR` - chyba pri inicializácii

---

### 2. RTC_SetClock()
```c
HAL_StatusTypeDef RTC_SetClock(const char *datetime_str);
```
Nastaví aktuálny čas a dátum v RTC.

**Parametre:**
- `datetime_str` - reťazec vo formáte: `"HH:MM:SS_RR.MM.DD"`
  - `HH` - hodiny (00-23, 24-hodinový formát)
  - `MM` - minúty (00-59)
  - `SS` - sekundy (00-59)
  - `RR` - rok (00-99, predstavuje 2000-2099)
  - `MM` - mesiac (01-12)
  - `DD` - deň (01-31)

**Návratová hodnota:**
- `HAL_OK` - čas úspešne nastavený
- `HAL_ERROR` - chyba (nesprávny formát alebo neplatné hodnoty)

---

### 3. RTC_ReadClock()
```c
HAL_StatusTypeDef RTC_ReadClock(char *datetime_str);
```
Prečíta aktuálny čas a dátum z RTC.

**Parametre:**
- `datetime_str` - buffer pre výsledný reťazec (minimálne 18 bajtov)

**Výstupný formát:** `"HH:MM:SS_RR.MM.DD"` (rovnaký ako pri nastavovaní)

**Návratová hodnota:**
- `HAL_OK` - čas úspešne prečítaný
- `HAL_ERROR` - chyba pri čítaní

---

### 4. RTC_SetAlarm()
```c
HAL_StatusTypeDef RTC_SetAlarm(const char *alarm_str, uint8_t duration_sec);
```
Nastaví denný alarm s časom trvania.

**Parametre:**
- `alarm_str` - reťazec vo formáte: `"HH:MM:SS"` alebo `"0"` pre vypnutie
  - `HH` - hodiny (00-23, 24-hodinový formát)
  - `MM` - minúty (00-59)
  - `SS` - sekundy (00-59)
- `duration_sec` - trvanie alarmu v sekundách (1-255), 0 = vypnúť alarm

**Návratová hodnota:**
- `HAL_OK` - alarm úspešne nastavený
- `HAL_ERROR` - chyba (nesprávny formát alebo neplatné hodnoty)

**Príklady:**
- `RTC_SetAlarm("07:00:00", 120)` - alarm o 7:00 na 2 minúty (120s)
- `RTC_SetAlarm("0", 0)` - vypni alarm
- `RTC_SetAlarm("08:30:00", 255)` - alarm o 8:30 na 255 sekúnd

**Správanie:**
- Alarm sa spustí v nastavenom čase
- Callback sa volá každú sekundu počas trvania
- Po uplynutí `duration_sec` sekúnd sa alarm automaticky vypne

---

### 5. RTC_AlarmFunc()
```c
void RTC_AlarmFunc(RTC_AlarmCallback_t callback);
```
Registruje callback funkciu pre alarm (krátky názov).

**Parametre:**
- `callback` - ukazovateľ na funkciu typu `void (*)(void)`

**Príklad callback funkcie:**
```c
void MyAlarm(void)
{
    // Volá sa každú sekundu počas alarmu
    BEEP(1000, 50, 1.0);  // Krátke pípnutie
    LED_ParseCommand("R255G0B0");  // Červená LED
}
```

---

## Príklad použitia

### Základné použitie (bez alarmu)

```c
#include "rtc.h"
#include <stdio.h>

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    
    // 1. Inicializácia RTC
    if (RTC_Init() != HAL_OK)
    {
        // Chyba inicializácie
        Error_Handler();
    }
    
    // 2. Nastavenie času a dátumu
    // Nastaví: 14:30:45, 14. december 2025
    if (RTC_SetClock("14:30:45_25.12.14") != HAL_OK)
    {
        // Chyba pri nastavovaní času
        Error_Handler();
    }
    
    // 3. Čítanie času
    char datetime[RTC_DATETIME_STRING_SIZE];
    
    while (1)
    {
        if (RTC_ReadClock(datetime) == HAL_OK)
        {
            printf("Aktuálny čas: %s\n", datetime);
            // Výstup: "Aktuálny čas: 14:30:45_25.12.14"
        }
        
        HAL_Delay(1000);  // Čítaj každú sekundu
    }
}
```

### Použitie s alarmom

```c
#include "rtc.h"
#include "led.h"
#include "alarm.h"

// Callback funkcia pre alarm - volá sa každú sekundu
void MyAlarm(void)
{
    // Pípaj každú sekundu počas alarmu
    BEEP(1000, 80, 0.5);
    LED_ParseCommand("R255G0B0W0#30");  // Červené LED
}

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    
    // Inicializácia periférií
    RTC_Init();
    LED_Init(&hlptim2, LPTIM_CHANNEL_1);
    BEEP_Init(&hlptim3, LPTIM_CHANNEL_3);
    
    // Nastavenie času
    RTC_SetClock("06:55:00_25.12.14");
    
    // Registrácia callback funkcie
    RTC_AlarmFunc(MyAlarm);
    
    // Nastavenie alarmu na 7:00:00, trvanie 120 sekúnd (2 minúty)
    RTC_SetAlarm("07:00:00", 120);
    
    char datetime[RTC_DATETIME_STRING_SIZE];
    
    while (1)
    {
        RTC_ReadClock(datetime);
        printf("Čas: %s\n", datetime);
        HAL_Delay(1000);
    }
}
```

### Riadenie alarmu

```c
// Nastaviť alarm na 8:30 na 60 sekúnd (1 minúta)
RTC_SetAlarm("08:30:00", 60);

// Nastaviť alarm na maximum (255 sekúnd = 4 min 15 sec)
RTC_SetAlarm("07:00:00", 255);

// Vypnúť alarm
RTC_SetAlarm("0", 0);

// Zmeniť čas alarmu (starý sa automaticky vypne)
RTC_SetAlarm("09:00:00", 180);  // 9:00 na 3 minúty
```

## Príklady formátu

### Čas a dátum
| Popis | Formát |
|-------|--------|
| Polnoc, 1. január 2025 | `"00:00:00_25.01.01"` |
| Poludnie, 14. december 2025 | `"12:00:00_25.12.14"` |
| Večer, 31. december 2099 | `"23:59:59_99.12.31"` |

### Alarm
| Popis | Príkaz |
|-------|--------|
| Alarm o 7:00 na 2 minúty | `RTC_SetAlarm("07:00:00", 120)` |
| Alarm o 8:30 na 1 minútu | `RTC_SetAlarm("08:30:00", 60)` |
| Alarm o 12:00 na max čas | `RTC_SetAlarm("12:00:00", 255)` |
| Vypnúť alarm | `RTC_SetAlarm("0", 0)` |

## Dôležité poznámky

1. **Externý krištáľ:** Driver vyžaduje pripojený 32.768 kHz LSE krištáľ na pinoch PC14/PC15
2. **Backup doména:** RTC si udržuje čas aj po resete MCU (pokiaľ je napájanie)
3. **Prvé spustenie:** Pri prvom zapnutí je potrebné nastaviť čas pomocou `RTC_SetClock()`
4. **Buffer size:** Buffer pre `RTC_ReadClock()` musí mať minimálne `RTC_DATETIME_STRING_SIZE` (18) bajtov
5. **Format validation:** `RTC_SetClock()` kontroluje platnosť vstupných hodnôt
6. **Alarm trvanie:** Callback sa volá každú sekundu počas trvania (1-255 sekúnd)
7. **Automatické vypnutie:** Alarm sa automaticky vypne po uplynutí `duration_sec`
8. **Vypnutie alarmu:** `RTC_SetAlarm("0", 0)` okamžite vypne alarm
9. **Callback z ISR:** Alarm callback sa volá z interrupt handlera - drž ho krátky!
10. **IRQ Handler:** `RTC_TAMP_IRQHandler` je automaticky pripojený v `stm32u0xx_it.c`

## Technické detaily

- **LSE frekvencia:** 32.768 kHz
- **Asynchrónny preddelič:** 127
- **Synchrónny preddelič:** 255
- **Výsledná frekvencia:** 1 Hz (1 tick = 1 sekunda)
- **Formát hodín:** 24-hodinový
