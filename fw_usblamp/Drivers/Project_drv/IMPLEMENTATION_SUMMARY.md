# SK6812RGBW LED Driver - Súhrn implementácie

## Implementované súbory

### Driver súbory
1. **led.h** - Hlavičkový súbor s API a definíciami
2. **led.c** - Implementácia drivera s PWM+DMA riadením
3. **led_examples.h** - Hlavičkový súbor s príkladmi
4. **led_examples.c** - Ukážkové efekty a použitie

### Modifikované súbory
1. **main.c** - Pridané:
   - Include led.h
   - DMA handle deklarácia (hdma_tim2_ch1)
   - MX_DMA_Init() funkcia
   - LED_Init() v USER CODE BEGIN 2
   - HAL_TIM_PWM_PulseFinishedCallback()

2. **stm32u0xx_hal_msp.c** - Aktualizované:
   - HAL_TIM_Base_MspInit() - pridaná DMA konfigurácia
   - HAL_TIM_MspPostInit() - zmenená konfigurácia z PA15 na PA8

3. **stm32u0xx_it.c** - Pridané:
   - extern DMA_HandleTypeDef hdma_tim2_ch1
   - DMA1_Channel1_IRQHandler()

## Technické parametre

### Hardvér
- **MCU**: STM32U073KCU (48MHz)
- **LED**: 30x SK6812MINI-RGBW-NW-P6
- **Data pin**: PA8 (TIM2_CH1)
- **Power control**: PB5 (CTL_LEN)

### Timing konfigurácia
- **Timer**: TIM2, 48MHz, Period=60 (800kHz)
- **Bit 0**: Duty 20/60 (~300ns HIGH)
- **Bit 1**: Duty 40/60 (~833ns HIGH)
- **Reset**: 80+ pulzov LOW (>100μs)

### DMA konfigurácia
- **DMA1 Channel 1**
- **Request**: TIM2_CH1
- **Transfer**: Memory to Peripheral, Normal mode
- **Priority**: High
- **Buffer size**: 1040 words (960 LED data + 80 reset)

## API prehľad

### Základné funkcie
```c
LED_Init(&htim2, TIM_CHANNEL_1);     // Inicializácia
LED_PowerOn();                        // Zapnutie s 500ms delay
LED_PowerOff();                       // Vypnutie
```

### Nastavenie farieb
```c
LED_SetColor(pos, &color);           // Jedna LED
LED_SetColors(positions, count, &color); // Viacero LED
LED_SetRange(start, end, &color);    // Rozsah LED
LED_Update();                         // Odoslanie dát
LED_AllOff();                        // Vypnúť všetky
```

### Príkazy
```c
LED_ParseCommand("L_1_r_255");       // LED 1, červená, max
LED_ParseCommand("L_1-5_g_128");     // LED 1-5, zelená, 50%
LED_ParseCommand("L_1,3,5_b_200");   // LED 1,3,5, modrá
LED_ParseCommand("L_OFF");           // Vypnúť napájanie
```

### Štruktúry
```c
typedef struct {
    uint8_t r;  // Červená (0-255)
    uint8_t g;  // Zelená (0-255)
    uint8_t b;  // Modrá (0-255)
    uint8_t w;  // Biela (0-255)
} LED_Color_t;
```

## Príklady použitia

### Jednoduchý test
```c
LED_PowerOn();
LED_Color_t red = {255, 0, 0, 0};
LED_SetColor(1, &red);
LED_Update();
```

### Dúhový efekt
```c
LED_Example_Rainbow();  // Zavolá dúhový efekt
```

### Všetky dema
```c
LED_RunAllExamples();   // Spustí všetky efekty
```

## Integrácia do projektu

### 1. Pridanie do build systému
Uistite sa, že sú pridané do kompilácie:
- `Drivers/Project_drv/led.c`
- `Drivers/Project_drv/led_examples.c` (voliteľné)

### 2. Include cesty
Pridajte cestu:
- `Drivers/Project_drv`

### 3. V main.c
```c
#include "../Drivers/Project_drv/led.h"
// Voliteľne pre príklady:
// #include "../Drivers/Project_drv/led_examples.h"

// V main() po inicializácii:
LED_Init(&htim2, TIM_CHANNEL_1);

// Pre test:
// LED_Example_BasicTest();
// alebo
// LED_RunAllExamples();
```

### 4. HAL konfigurácia
Uistite sa, že sú povolené v `stm32u0xx_hal_conf.h`:
- HAL_TIM_MODULE_ENABLED
- HAL_DMA_MODULE_ENABLED
- HAL_GPIO_MODULE_ENABLED

## Riešenie problémov

### LED nefungujú vôbec
1. Skontrolujte napájanie (CTL_LEN pin)
2. Overťe TIM2 konfiguráciu
3. Skontrolujte DMA inicializáciu
4. Overťe PA8 alternate function

### Nesprávne farby
1. Overte GRB poradie v led.c
2. Skontrolujte timing (Period=60 pre 48MHz)
3. Upravte duty cycles (LED_BIT_0_DUTY, LED_BIT_1_DUTY)

### Blikanie/nestabilita
1. Skontrolujte kvalitu napájania
2. Pridajte kondenzátor na napájanie LED
3. Upravte LED_RESET_PULSES (skúste 100-120)

### DMA errory
1. Overte že DMA1_CLK je zapnuté
2. Skontrolujte NVIC priority
3. Overte buffer alignment (4-byte)

## Ďalšie rozšírenia

### Možné vylepšenia
1. **Jasové riadenie**: Globálny brightness parameter
2. **Gamma korekcia**: Pre lineárne vnímanie jasu
3. **Animácie**: Plynulé prechody medzi farbami
4. **Efekty**: Viac preddefinovaných efektov
5. **Non-blocking**: Plne asynchrónne animácie
6. **Power management**: Meranie spotreby, sleep mode

### Príklad gamma korekcie
```c
const uint8_t gamma8[] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  1,  1,  1,  1,
    // ... pokračovanie tabuľky
};

color.r = gamma8[linear_r];
```

## Licencia a kredit
- **Autor**: orgo
- **Dátum**: December 13, 2025
- **Projekt**: SmartLamp - Inteligentná programovateľná lampa
- **MCU**: STM32U073KCU
- **LED**: SK6812MINI-RGBW-NW-P6

## Referencie
- SK6812 datasheet
- STM32U0 reference manual
- HAL driver documentation
