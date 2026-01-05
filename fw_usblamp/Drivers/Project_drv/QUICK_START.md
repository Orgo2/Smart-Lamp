# Quick Start Guide - SK6812RGBW LED Driver

## Rýchly štart

### Hardvér
- **30x SK6812MINI-RGBW-NW-P6** LED diody
- **4 kanály**: Červená (R), Zelená (G), Modrá (B), **Biela (W)**
- **Dátový pin**: PA8 (TIM2_CH1)
- **Power control**: PB5 (CTL_LEN)

### Základné použitie

```c
#include "../Drivers/Project_drv/led.h"

int main(void)
{
    // ... HAL init ...
    
    LED_Init(&htim2, TIM_CHANNEL_1);
    LED_PowerOn();  // Čaká 500ms na stabilizáciu
    
    // Príklad: Prvé 3 LED červené, ďalšie 3 biele, ďalšie 3 modré
    LED_Color_t red = {255, 0, 0, 0};      // R=255, G=0, B=0, W=0
    LED_Color_t white = {0, 0, 0, 255};    // R=0, G=0, B=0, W=255
    LED_Color_t blue = {0, 0, 255, 0};     // R=0, G=0, B=255, W=0
    
    LED_SetRange(1, 3, &red);    // LED 1-3: červená - POSIELA VŠETKÝCH 30 LED
    LED_SetRange(4, 6, &white);  // LED 4-6: biela - POSIELA VŠETKÝCH 30 LED
    LED_SetRange(7, 9, &blue);   // LED 7-9: modrá - POSIELA VŠETKÝCH 30 LED
    
    // Každá funkcia automaticky posiela dáta VŠETKÝM 30 LED!
    
    while (1) { }
}
```

## ⚠️ DÔLEŽITÉ - Automatické posielanie

**KAŽDÁ funkcia automaticky posiela dáta VŠETKÝM 30 LED!**

```c
LED_SetColor(1, &red);    // Nastaví LED 1 a POŠLE dáta všetkým 30 LED
LED_SetColor(2, &green);  // Nastaví LED 2 a POŠLE dáta všetkým 30 LED
LED_SetColor(3, &blue);   // Nastaví LED 3 a POŠLE dáta všetkým 30 LED
```

Každá LED si pamätá predchádzajúcu hodnotu, ostatné zostanú ako boli nastavené.

## RGBW - Štyri kanály

### LED_Color_t štruktúra
```c
typedef struct {
    uint8_t r;  // Červená 0-255
    uint8_t g;  // Zelená 0-255
    uint8_t b;  // Modrá 0-255
    uint8_t w;  // BIELA 0-255  ← ŠTVRTÝ KANÁL!
} LED_Color_t;
```

### Príklady farieb

```c
// Červená
LED_Color_t red = {.r=255, .g=0, .b=0, .w=0};

// Zelená
LED_Color_t green = {.r=0, .g=255, .b=0, .w=0};

// Modrá
LED_Color_t blue = {.r=0, .g=0, .b=255, .w=0};

// Biela (W kanál) - SAMOSTATNÁ BIELA LED!
LED_Color_t white_pure = {.r=0, .g=0, .b=0, .w=255};

// RGB biela (zmiešaná)
LED_Color_t white_rgb = {.r=255, .g=255, .b=255, .w=0};

// Biela + RGB (maximálny jas)
LED_Color_t white_max = {.r=255, .g=255, .b=255, .w=255};

// Teplá biela (RGB mix + W)
LED_Color_t warm_white = {.r=255, .g=147, .b=41, .w=200};

// Studená biela
LED_Color_t cool_white = {.r=201, .g=226, .b=255, .w=200};
```

## Reťazenie príkazov

```c
// Prvé 3 na červenú, ďalšie 3 na bielu, ďalšie 3 na modrú
LED_Color_t red = {255, 0, 0, 0};
LED_Color_t white = {0, 0, 0, 255};
LED_Color_t blue = {0, 0, 255, 0};

LED_SetRange(1, 3, &red);     // LED 1-3 červené
LED_SetRange(4, 6, &white);   // LED 4-6 biele (W kanál)
LED_SetRange(7, 9, &blue);    // LED 7-9 modré
// LED 10-30 zostanú v predchádzajúcom stave (alebo nuly ak neboli nastavené)
```

## Príkazy - Parser

### Formát
```
L_<pozície>_<kanál>_<hodnota>
```

### Kanály
- `r` - Červená
- `g` - Zelená  
- `b` - Modrá
- `w` - **Biela (W kanál)**

### Príklady

```c
// Základné
LED_ParseCommand("L_1_r_255");        // LED 1: červená max
LED_ParseCommand("L_2_w_255");        // LED 2: biela W kanál max
LED_ParseCommand("L_3_b_128");        // LED 3: modrá 50%

// Rozsahy
LED_ParseCommand("L_1-3_r_255");      // LED 1-3: červené
LED_ParseCommand("L_4-6_w_255");      // LED 4-6: biele (W)
LED_ParseCommand("L_7-9_b_200");      // LED 7-9: modré

// Zoznam
LED_ParseCommand("L_1,5,10_w_255");   // LED 1,5,10: biele

// Vypnutie
LED_ParseCommand("L_OFF");            // Vypne napájanie
```

### Reťazenie príkazov
```c
LED_ParseCommand("L_1-3_r_255");    // Prvé 3 červené
LED_ParseCommand("L_4-6_w_255");    // Ďalšie 3 biele  
LED_ParseCommand("L_7-9_b_255");    // Ďalšie 3 modré
// Každý príkaz posiela dáta všetkým 30 LED!
```

## API Funkcie

### Inicializácia
```c
LED_Init(&htim2, TIM_CHANNEL_1);
LED_PowerOn();   // 500ms delay
```

### Nastavenie farieb (automaticky posiela)
```c
LED_SetColor(position, &color);           // Jedna LED
LED_SetRange(start, end, &color);         // Rozsah
LED_SetColors(positions, count, &color);  // Viacero
```

### Manuálne posielanie (ak potrebné)
```c
LED_Update();    // Pošle aktuálny buffer všetkým LED
```

### Vypnutie
```c
LED_AllOff();    // Všetky LED na 0 a pošle
LED_PowerOff();  // Vypne napájanie CTL_LEN
```

## Praktický príklad

```c
LED_PowerOn();

// Vytvor farebný vzor
LED_Color_t red = {255, 0, 0, 0};
LED_Color_t green = {0, 255, 0, 0};
LED_Color_t blue = {0, 0, 255, 0};
LED_Color_t white = {0, 0, 0, 255};

// Prvých 10: červené
LED_SetRange(1, 10, &red);

// Ďalších 10: biele (W kanál!)
LED_SetRange(11, 20, &white);

// Posledných 10: modré
LED_SetRange(21, 30, &blue);

// Teraz zmeň len LED 15 na zelenú (ostatné zostanú)
LED_SetColor(15, &green);
```

## Troubleshooting

### LED nefungujú
1. Zavolali ste `LED_PowerOn()`?
2. Je CTL_LEN (PB5) HIGH?
3. Skontrolujte napájanie LED

### Nesprávne farby
1. Overte RGBW hodnoty - **štvrtý parameter je W!**
2. SK6812RGBW má 4 kanály, nie 3
3. Timing je 800kHz (Period=60 @ 48MHz)

### Biela LED nefunguje
```c
// ❌ ZLE - biela RGB mix
LED_Color_t wrong = {255, 255, 255, 0};

// ✅ SPRÁVNE - biela W kanál
LED_Color_t correct = {0, 0, 0, 255};
```

---

**Driver je optimalizovaný pre RGBW a automatické reťazenie!**
