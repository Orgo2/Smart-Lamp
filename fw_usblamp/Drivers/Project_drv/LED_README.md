# SK6812RGBW LED Driver - Dokumentácia

## Prehľad
Driver pre riadenie 30ks SK6812MINI-RGBW-NW-P6 digitálnych LED diód pomocou STM32U073 MCU.

## Hardvér
- **MCU**: STM32U073KCU (48MHz)
- **LED typ**: SK6812MINI-RGBW-NW-P6 (30 kusov)
- **Kanály**: R, G, B, **W** (4 kanály, nie 3!)
- **Dátový pin**: PA8 (LED_Pin) - TIM2_CH1
- **Power control**: PB5 (CTL_LEN_Pin)
- **Protokol**: PWM + DMA (800kHz)

## Časovanie SK6812RGBW
- **Bit 0**: ~300ns HIGH, ~900ns LOW
- **Bit 1**: ~833ns HIGH, ~417ns LOW  
- **Reset**: >80μs LOW
- **Poradie**: G-R-B-W (zelená, červená, modrá, **biela**)
- **4 bajty na LED** (nie 3!)

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

### Príklady použitia W kanála
```c
// Čisto biela LED (len W kanál)
LED_Color_t white_pure = {0, 0, 0, 255};

// RGB biela (bez W)
LED_Color_t white_rgb = {255, 255, 255, 0};

// Maximálny jas (RGB + W)
LED_Color_t white_max = {255, 255, 255, 255};

// Teplá biela
LED_Color_t warm = {255, 147, 41, 200};

// Studená biela
LED_Color_t cool = {201, 226, 255, 200};
```

## ⚠️ DÔLEŽITÉ - Automatické posielanie

**KAŽDÁ funkcia automaticky posiela dáta VŠETKÝM 30 LED!**

```c
LED_SetColor(1, &red);     // Pošle dáta všetkým LED
LED_SetRange(1, 10, &blue); // Pošle dáta všetkým LED
```

Nemusíte volať `LED_Update()` - to sa deje automaticky!

## API Funkcie

### Inicializácia
```c
LED_Init(&htim2, TIM_CHANNEL_1);
LED_PowerOn();  // Čaká 500ms
```

### Nastavenie farieb (automaticky posiela)
```c
// Jedna LED
LED_SetColor(position, &color);

// Rozsah LED
LED_SetRange(start, end, &color);

// Viacero konkrétnych LED
uint8_t leds[] = {1, 5, 10};
LED_SetColors(leds, 3, &color);
```

### Vypnutie
```c
LED_AllOff();     // Všetky LED na 0
LED_PowerOff();   // Vypne napájanie
```

### Príkazy
```c
LED_ParseCommand("L_1_r_255");      // LED 1 červená
LED_ParseCommand("L_1-10_w_255");   // LED 1-10 biele (W!)
LED_ParseCommand("L_1,5,10_b_128"); // LED 1,5,10 modré
LED_ParseCommand("L_OFF");          // Vypni napájanie
```

## Reťazenie príkazov

```c
LED_PowerOn();

LED_Color_t red = {255, 0, 0, 0};
LED_Color_t white = {0, 0, 0, 255};
LED_Color_t blue = {0, 0, 255, 0};

// Každý príkaz automaticky posiela všetkých 30 LED
LED_SetRange(1, 10, &red);      // LED 1-10 červené
LED_SetRange(11, 20, &white);   // LED 11-20 biele (W kanál!)
LED_SetRange(21, 30, &blue);    // LED 21-30 modré

// LED 1-30 sú teraz nastavené podľa vyššie uvedeného
```

## Príklady

### Základné použitie
```c
#include "../Drivers/Project_drv/led.h"

LED_Init(&htim2, TIM_CHANNEL_1);
LED_PowerOn();

LED_Color_t red = {255, 0, 0, 0};
LED_SetColor(1, &red);  // Automaticky pošle
```

### Farebný vzor
```c
LED_Color_t red = {255, 0, 0, 0};
LED_Color_t white = {0, 0, 0, 255};
LED_Color_t blue = {0, 0, 255, 0};

LED_SetRange(1, 10, &red);
LED_SetRange(11, 20, &white);
LED_SetRange(21, 30, &blue);
```

### Použitie príkazov
```c
LED_ParseCommand("L_1-10_r_255");
LED_ParseCommand("L_11-20_w_255");
LED_ParseCommand("L_21-30_b_255");
```

## Technické detaily

### TIM2 konfigurácia
- Frekvencia: 48MHz
- Prescaler: 0
- Period: 59
- PWM: 800kHz

### DMA
- DMA1 Channel 1
- TIM2_CH1
- Normal mode
- High priority

### Buffer
- 30 LED × 4 bajty × 8 bitov = 960 PWM hodnôt
- + 80 reset pulzov = 1040 celkom

## Riešenie problémov

### LED nefungujú
1. Zavolali ste `LED_PowerOn()`?
2. Je CTL_LEN (PB5) HIGH?
3. Je napájanie zapojené?

### Biela nefunguje
```c
// ❌ ZLE - RGB mix
LED_Color_t wrong = {255, 255, 255, 0};

// ✅ SPRÁVNE - W kanál
LED_Color_t correct = {0, 0, 0, 255};
```

---

**Driver je pripravený s plnou RGBW podporou a automatickým reťazením!**
