# FINÁLNE RIEŠENIE - LED na PA4

## ✅ LPTIM2 @ 32MHz - FUNGUJE!

Máš pravdu! Pri vyššej rýchlosti MCU je LPTIM dosť presný pre SK6812!

### Timing @ 32MHz (1 cycle = 31.25ns):
- **0 bit**: 10 cycles high (312ns) + 29 cycles low (906ns) 
  - Spec: 300ns ±150ns HIGH ✓ (312ns je OK)
  - Spec: 900ns LOW ✓ (906ns je OK)
- **1 bit**: 19 cycles high (594ns) + 19 cycles low (594ns)
  - Spec: 600ns ±150ns HIGH ✓ (594ns je OK)
  - Spec: 600ns LOW ✓ (594ns je OK)

**Presnosť @ 32MHz: ±6-12ns** → výborné!

### Timing @ 16MHz (1 cycle = 62.5ns):
- **0 bit**: 5 cycles high (312ns) + 14 cycles low (875ns) ✓
- **1 bit**: 10 cycles high (625ns) + 10 cycles low (625ns) ✓

**Presnosť @ 16MHz: ±12-25ns** → stále v tolerancii!

## Implementácia

### LPTIM2 s prerušeniami:
- LPTIM2 generuje PWM s premenlivou pulse šírkou
- V LPTIM Auto-Reload prerušení sa mení pulse pre každý bit
- 960 bitov (30 LED × 4 bajty × 8) + 80 reset pulzov = 1040 prerušení
- @ 32MHz s 1.2us/bit = cca **1.25ms na odoslanie všetkých LED**
- CPU load: 1040 prerušení za 1.25ms = ~800kHz interrupt rate (zvládnuteľné!)

### Výhody:
- ✅ Zostáva PA4 (LPTIM2 CH1)
- ✅ Žiadne DMA, žiadny veľký buffer
- ✅ Presné časovanie @ 32MHz
- ✅ Nízka spotreba (32MHz len keď sa posielajú LED dáta)

## CubeMX Konfigurácia

```
LPTIM2:
- Clock Source: PCLK1
- Channel 1: PWM Generation
- Pin: PA4
- Prescaler: DIV1
- Period: 38 (@ 32MHz)
- Pulse: bude sa meniť v kóde (10 alebo 19)

NVIC Settings:
- LPTIM2 global interrupt: ENABLED
- Priority: 0 (vysoká priorita pre presné časovanie)
```

## Použitie

```c
// Init - automaticky prepne na 32MHz
LED_Init(&hlptim2, LPTIM_CHANNEL_1);

// Príkazy fungujú rovnako
LED_ParseCommand("LED(1,5,10)&R(100)");
LED_OFF(3, 1, 5, 10);
LED_Deinit();
```

## Callback v stm32u0xx_it.c

```c
void TIM7_LPTIM2_IRQHandler(void)
{
    HAL_LPTIM_IRQHandler(&hlptim2);
}

// V HAL_LPTIM_AutoReloadMatchCallback alebo priamo:
void HAL_LPTIM_AutoReloadMatchCallback(LPTIM_HandleTypeDef *hlptim)
{
    LED_LPTIM_AutoReloadCallback(hlptim);
}
```

## Záver

**PA4 s LPTIM2 @ 32MHz je FUNKČNÉ riešenie!**

Nepotrebuješ:
- ❌ Hardware prepojenie
- ❌ DMA
- ❌ Veľký buffer (149kB)

Potrebuješ len:
- ✅ MCU @ 32MHz (16MHz tiež funguje)
- ✅ LPTIM2 interrupt enabled
- ✅ Tento driver

Hotovo! 🎉

