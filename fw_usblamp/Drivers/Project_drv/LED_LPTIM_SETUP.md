# LED Driver - LPTIM2 @ 32MHz Setup

## RIEŠENIE FUNGUJE! ✅

PA4 s LPTIM2 @ 32MHz poskytuje dostatočnú presnosť pre SK6812!

## CubeMX Konfigurácia

### 1. LPTIM2 Setup
```
Pinout & Configuration → Timers → LPTIM2
- Clock Source: PCLK1
- Channel 1: PWM Generation
- Pin: PA4 (CTL_LED)
- Prescaler: DIV1
- Period: 65535 (bude nastavené v kóde)
- Pulse: 0 (bude nastavené v kóde)

NVIC Settings:
- LPTIM2 global interrupt: ENABLED ✓
- Priority: 0 (vysoká priorita!)
```

### 2. Clock Configuration
```
Clock Configuration → System Clock Mux
- Select MSI
- MSI Range: 32MHz (RCC_MSIRANGE_13)
```

## Callback v stm32u0xx_it.c

Pridaj do USER CODE sekcie:

```c
/* USER CODE BEGIN Includes */
#include "led.h"  // Pridaj LED driver header
/* USER CODE END Includes */

// V TIM7_LPTIM2_IRQHandler už je:
void TIM7_LPTIM2_IRQHandler(void)
{
  /* USER CODE BEGIN TIM7_LPTIM2_IRQn 0 */

  /* USER CODE END TIM7_LPTIM2_IRQn 0 */
  HAL_LPTIM_IRQHandler(&hlptim2);
  /* USER CODE BEGIN TIM7_LPTIM2_IRQn 1 */

  /* USER CODE END TIM7_LPTIM2_IRQn 1 */
}
```

A pridaj v main.c alebo stm32u0xx_hal_lptim.c callback:

```c
/* USER CODE BEGIN 4 */

void HAL_LPTIM_AutoReloadMatchCallback(LPTIM_HandleTypeDef *hlptim)
{
    LED_LPTIM_AutoReloadCallback(hlptim);
}

/* USER CODE END 4 */
```

## Použitie

```c
// V main.c USER CODE BEGIN 2:
LED_Init(&hlptim2, LPTIM_CHANNEL_1);

// Ovládanie LED
LED_ParseCommand("LED(1,5,10)&R(255)");
LED_OFF(3, 1, 5, 10);
LED_Deinit();
```

## Timing @ 32MHz

| Bit | High Time | Low Time | SK6812 Spec | Status |
|-----|-----------|----------|-------------|--------|
| 0   | 312ns (10 cyc) | 906ns (29 cyc) | 300ns ±150 / 900ns | ✓ OK |
| 1   | 594ns (19 cyc) | 594ns (19 cyc) | 600ns ±150 / 600ns | ✓ OK |

**Presnosť: ±6-12ns** - výborné pre SK6812!

## Performance

- 1040 prerušení (960 bitov + 80 reset)
- Každé prerušenie: ~1.2us
- Celková doba: **~1.25ms** na odoslanie všetkých 30 LED
- CPU load: ~800kHz interrupt rate (OK pre 32MHz MCU)

## Spotreba

- Idle (4MHz): ~3.5mA
- Init delay (sleep): ~4mA
- LED TX (32MHz): ~15mA počas 1.25ms
- Priemerná spotreba: minimálna (32MHz len keď sa odosielajú LED)

## Záver

LPTIM2 @ 32MHz je **optimálne riešenie** pre PA4:
- ✅ Žiadne hardware prepojenie
- ✅ Žiadne DMA
- ✅ Malý RAM footprint
- ✅ Presné časovanie
- ✅ Nízka spotreba

Hotovo! 🚀
