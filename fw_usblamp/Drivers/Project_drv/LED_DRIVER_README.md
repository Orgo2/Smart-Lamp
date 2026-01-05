# SK6812RGBW LED Driver Documentation

## Overview
Driver pre riadenie 30ks SK6812MINI-RGBW-NW-P6 digitálnych LED diód pomocou STM32U073 MCU.

## Hardvérová konfigurácia
- **MCU**: STM32U073KCU
- **LED typ**: SK6812MINI-RGBW-NW-P6 (30 kusov)
- **Dátový pin**: PA8 (LED_Pin) - TIM2_CH1
- **Power control**: PB5 (CTL_LEN_Pin)
- **Komunikačný protokol**: PWM + DMA (800kHz, 1.25μs period)

## Časovanie SK6812
- **Bit 0**: ~300ns HIGH, ~900ns LOW (duty cycle ~25%)
- **Bit 1**: ~600ns HIGH, ~600ns LOW (duty cycle ~50%)
- **Reset**: >80μs LOW
- **Poradie dát**: G-R-B-W (zelená, červená, modrá, biela)

## API Funkcie

### Inicializácia
```c
HAL_StatusTypeDef LED_Init(TIM_HandleTypeDef *htim, uint32_t channel);
```
Inicializuje LED driver s časovačom a kanálom.
- **Parametre**: 
  - `htim`: Pointer na TIM handle (TIM2)
  - `channel`: TIM kanál (TIM_CHANNEL_1)
- **Návratová hodnota**: HAL_OK alebo HAL_ERROR

### Zapnutie napájania
```c
HAL_StatusTypeDef LED_PowerOn(void);
```
Zapne napájanie LED (CTL_LEN pin HIGH) a čaká 500ms na stabilizáciu.

### Vypnutie napájania
```c
HAL_StatusTypeDef LED_PowerOff(void);
```
Vypne všetky LED a potom vypne napájanie (CTL_LEN pin LOW).

### Nastavenie farby jednej LED
```c
HAL_StatusTypeDef LED_SetColor(uint8_t position, LED_Color_t *color);
```
Nastaví farbu pre konkrétnu LED.
- **Parametre**:
  - `position`: Pozícia LED (1-30)
  - `color`: Pointer na LED_Color_t štruktúru s RGBW hodnotami (0-255)

### Nastavenie farby viacerých LED
```c
HAL_StatusTypeDef LED_SetColors(uint8_t *positions, uint8_t count, LED_Color_t *color);
```
Nastaví rovnakú farbu pre viacero LED.

### Nastavenie rozsahu LED
```c
HAL_StatusTypeDef LED_SetRange(uint8_t start_pos, uint8_t end_pos, LED_Color_t *color);
```
Nastaví farbu pre rozsah LED (napr. LED 5-10).

### Aktualizácia LED
```c
HAL_StatusTypeDef LED_Update(void);
```
Odošle aktuálne dáta na LED pásik. Táto funkcia musí byť zavolaná po nastavení farieb.

### Vypnutie všetkých LED
```c
HAL_StatusTypeDef LED_AllOff(void);
```
Vypne všetky LED (nastaví ich na 0).

### Parsovanie príkazov
```c
HAL_StatusTypeDef LED_ParseCommand(const char *cmd);
```
Spracuje príkaz vo formáte:
- `L_OFF` - Vypne napájanie
- `L_<pozícia>_<farba>_<hodnota>` - Nastaví farbu

**Príklady príkazov:**
- `L_1_r_255` - LED 1, červená na maximum
- `L_1-5_g_128` - LED 1-5, zelená na 50%
- `L_1,3,5_b_200` - LED 1, 3, 5, modrá
- `L_10_w_100` - LED 10, biela
- `L_OFF` - Vypne napájanie

## Použitie

### Základný príklad
```c
#include "led.h"

// V main.c po inicializácii periférií:
LED_Init(&htim2, TIM_CHANNEL_1);

// Zapnúť napájanie
LED_PowerOn();

// Nastaviť prvú LED na červenú
LED_Color_t red = {.r = 255, .g = 0, .b = 0, .w = 0};
LED_SetColor(1, &red);
LED_Update();

// Nastaviť LED 5-10 na zelenú
LED_Color_t green = {.r = 0, .g = 255, .b = 0, .w = 0};
LED_SetRange(5, 10, &green);
LED_Update();

// Nastaviť všetky LED na teplú bielu
LED_Color_t warm_white = {.r = 255, .g = 147, .b = 41, .w = 200};
for (uint8_t i = 1; i <= 30; i++) {
    LED_SetColor(i, &warm_white);
}
LED_Update();

// Vypnúť všetky LED
LED_AllOff();

// Vypnúť napájanie
LED_PowerOff();
```

### Použitie s príkazmi
```c
// Parsovanie príkazov zo sériového portu alebo USB
LED_ParseCommand("L_1_r_255");      // LED 1 červená
LED_ParseCommand("L_1-10_g_128");   // LED 1-10 zelená
LED_ParseCommand("L_OFF");          // Vypnúť
```

### Dúhový efekt
```c
void rainbow_effect(void) {
    LED_PowerOn();
    
    for (uint8_t i = 1; i <= 30; i++) {
        LED_Color_t color;
        uint8_t hue = (i * 255) / 30;
        
        // Jednoduchá HSV to RGB konverzia
        if (hue < 85) {
            color.r = hue * 3;
            color.g = 255 - hue * 3;
            color.b = 0;
        } else if (hue < 170) {
            hue -= 85;
            color.r = 255 - hue * 3;
            color.g = 0;
            color.b = hue * 3;
        } else {
            hue -= 170;
            color.r = 0;
            color.g = hue * 3;
            color.b = 255 - hue * 3;
        }
        color.w = 0;
        
        LED_SetColor(i, &color);
    }
    LED_Update();
}
```

## Dôležité poznámky

1. **Napájanie**: Vždy najprv zavolajte `LED_PowerOn()` pred použitím LED. Driver automaticky čaká 500ms na stabilizáciu napájania.

2. **Aktualizácia**: Po nastavení farieb pomocou `LED_SetColor()` alebo podobných funkcií, musíte zavolať `LED_Update()` na odoslanie dát na LED pásik.

3. **DMA prenos**: Driver používa DMA pre efektívny prenos dát. Počas prenosu je stav nastavený na `LED_STATE_BUSY`.

4. **Callback**: `HAL_TIM_PWM_PulseFinishedCallback()` je už implementovaný v main.c a volá `LED_TIM_PulseFinishedCallback()`.

5. **Blokovanie**: Funkcia `LED_Update()` vráti `HAL_BUSY` ak práve prebieha prenos. Skontrolujte stav pomocou `LED_GetState()`.

6. **Vypnutie**: `L_OFF` vypne iba napájanie. Pre vypnutie LED ale ponechanie napájania použite `LED_AllOff()`.

## Technické detaily

### Konfigurácia TIM2
- **Frekvencia**: 48MHz
- **Prescaler**: 0
- **Period**: 59 (800kHz, 1.25μs)
- **PWM duty cycles**:
  - Bit 0: 20/60 (~33%, teoreticky 300ns)
  - Bit 1: 40/60 (~67%, teoreticky 833ns)

### DMA konfigurácia
- **DMA**: DMA1 Channel 1
- **Request**: TIM2_CH1
- **Direction**: Memory to Peripheral
- **Mode**: Normal (single shot)
- **Priority**: High

### Buffer veľkosť
- 30 LED × 4 bajty (RGBW) × 8 bitov = 960 PWM hodnôt
- + 80 reset pulzov = 1040 celkových hodnôt

## Riešenie problémov

1. **LED nefungujú**: Skontrolujte či je zapnuté napájanie pomocou `LED_PowerOn()`.

2. **Nesprávne farby**: Skontrolujte či ste zavolali `LED_Update()` po nastavení farieb.

3. **Blikajúce LED**: Možná rušenie alebo nestabilné napájanie. Skontrolujte kvalitu napájania.

4. **Prvá LED nesprávna farba**: Možné časovacie problémy. Overť konfiguráciu TIM2 a DMA.

5. **Kompilácia chyby**: Uistite sa, že máte správne includované všetky potrebné HAL moduly.
