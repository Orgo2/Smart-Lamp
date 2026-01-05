# LED Driver - GPIO Bit-banging s TIM2+DMA na PA4

## Riešenie
Keďže PA4 nemá TIM alternate function (len LPTIM2 bez DMA), použijeme:
- **TIM2** - generuje timing impulzy
- **DMA** - zapisuje do GPIO BSRR registra
- **PA4** - GPIO output riadený cez DMA

## CubeMX Konfigurácia

### 1. TIM2 Setup
```
Pinout & Configuration → Timers → TIM2
- Mode: Internal Clock
- Prescaler: 0
- Counter Period: 19 (pre 16MHz: 1.2us)
- Counter Mode: Up
- auto-reload preload: Disable

NVIC Settings:
- TIM2 global interrupt: DISABLED (nepoužívame prerušenia)

DMA Settings → Add DMA Request:
- Request: TIM2_UP
- Channel: DMA1 Channel 1 (alebo iný voľný)
- Direction: Memory To Peripheral
- Mode: Normal (nie Circular!)
- Increment Address: Memory increment enabled, Peripheral NO increment
- Data Width: Word to Word (32-bit)
```

### 2. GPIO PA4 Setup
```
Pinout & Configuration → System Core → GPIO
PA4:
- Mode: GPIO_Output
- GPIO output level: Low
- GPIO mode: Output Push Pull
- GPIO Pull-up/Pull-down: No pull-up and no pull-down
- Maximum output speed: Very High
- User Label: CTL_LED
```

### 3. DMA Setup Detail
```
V kóde musíš nastaviť:
- Peripheral Address = &GPIOA->BSRR (0x48000018)
- Memory Address = &led_gpio_buffer[0]
- Data Length = LED_BUFFER_SIZE + LED_RESET_PULSES
```

## Ako to funguje

1. **led_gpio_buffer[]** obsahuje príkazy pre GPIO (PA4_SET alebo PA4_RESET)
2. **TIM2** generuje Update Event každých 1.2us
3. **DMA** na každý Update Event zapíše jednu hodnotu z bufferu do GPIOA->BSRR
4. **GPIOA->BSRR** nastaví/resetuje PA4

### Timing @ 16MHz:
- 1 bit = 1.2us = 19 cyklov
- 0 bit: SET (1 cyklus DMA), potom RESET po 5 cykloch
- 1 bit: SET (1 cyklus DMA), potom RESET po 10 cykloch

**Problém:** Jednoduchý buffer s PA4_SET/RESET nebude fungovať správne lebo potrebujeme presnejšie časovanie HIGH/LOW fázy!

## Lepšie riešenie - Dvojitý buffer

Namiesto jednoduchého PA4_SET/RESET budeme musieť:
1. PA4_SET - začiatok bitu
2. čakať X cyklov (via TIM2 multiple updates)
3. PA4_RESET - koniec bitu

To znamená že **každý bit potrebuje 2 DMA transfery** a TIM2 musí mať kratší update period.

### Nová kalkulácia @ 16MHz:
- Timer period: ~100ns (1-2 cycles)
- 0 bit: SET → čakaj 3 update → RESET → čakaj 9 update
- 1 bit: SET → čakaj 6 update → RESET → čakaj 6 update

Toto je zložité! **Odporúčam 32MHz** pre lepší pomer timing/presnosť.

## Odporúčanie pre 32MHz

@ 32MHz: 1 cyklus = 31.25ns

SK6812 timing:
- 0 bit: 300ns high (10 cyklov), 900ns low (29 cyklov) 
- 1 bit: 600ns high (19 cyklov), 600ns low (19 cyklov)

Buffer pre každý bit:
```c
// 0 bit:
PA4_SET,   // 0ns
PA4_SET,   // ...
...        // (10x SET)
PA4_RESET, // 300ns
PA4_RESET, // ...
...        // (29x RESET)

// 1 bit:
PA4_SET,   // 0ns
...        // (19x SET)
PA4_RESET, // 600ns
...        // (19x RESET)
```

TIM2 period = 1 cyklus (31.25ns)
→ každý bit = 39 DMA transferov

To je **veľmi veľký buffer**: 30 LED × 4 bajty × 8 bitov × 39 = 37440 slov!
→ **149 kB RAM** - príliš veľa pre STM32U073!

## Finálne odporúčanie

**Jednoduchšie riešenie: Použiť SPI s DMA**

SPI môže byť remapped na PA4 a má presné časovanie s DMA support!

Pozri či PA4 má SPI1_NSS alebo SPI_MOSI alternate function v datasheete.

Ak nie, **musíš hardware prepájať PA4 → PA8** alebo použiť iný pin s TIM+DMA support.
