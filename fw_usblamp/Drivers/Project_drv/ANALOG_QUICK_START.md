# ANALOG Driver - Rýchly návod

## Základné použitie (3 kroky)

### 1. Include driver v main.c
```c
#include "analog.h"
```

### 2. Inicializácia (v main, po MX_ADC1_Init())
```c
ANALOG_Init(&hadc1);
```

### 3. Použitie v kóde
```c
// Získať intenzitu svetla v luxoch
float svetlo = ANALOG_GetLight();

// Získať napätie batérie vo voltoch
float bateria = ANALOG_GetBat();
```

## API prehľad

| Funkcia | Popis | Návratová hodnota |
|---------|-------|-------------------|
| `ANALOG_Init(&hadc1)` | Inicializuje driver a kalibruje ADC | void |
| `ANALOG_GetLight()` | Meria svetlo pomocou TIA a fotodiody | float (lux) |
| `ANALOG_GetBat()` | Meria napätie batérie cez delič | float (V) |

## Hardvér

### AN_LI (Light sensor)
- **Pin:** PA7 (ADC1_IN14)
- **TIA:** 330 kΩ feedback rezistor
- **Fotodioda:** SFH203P
- **Už nakonfigurované v CubeMX** ✓

### AN_BAT (Battery voltage)
- **Pin:** PB0 (ADC1_IN15) ✅ Nakonfigurované v kóde!
- **Delič:** R1=100kΩ, R2=47kΩ
- **Pripravené na použitie** - žiadna konfigurácia v CubeMX nie je potrebná

## Príklad integrácie v main.c

```c
#include "analog.h"

int main(void)
{
  HAL_Init();
  SystemClock_Config();
  MX_GPIO_Init();
  MX_ADC1_Init();
  // ... ostatné init ...
  
  // Inicializovať analog driver
  ANALOG_Init(&hadc1);
  
  // Inicializovať LED a ostatné drivery
  LED_Init(&hlptim2, LPTIM_CHANNEL_1);
  BEEP_Init(&hlptim3, LPTIM_CHANNEL_3);
  
  while (1)
  {
    // Meranie
    float lux = ANALOG_GetLight();
    float volt = ANALOG_GetBat();
    
    // Automatické riadenie LED podľa svetla
    if (lux < 100.0f)
    {
      LED_SetBrightness(100);  // Tma -> LED na max
    }
    else
    {
      LED_SetBrightness(0);    // Svetlo -> LED vypnuté
    }
    
    // Upozornenie pri nízkej batérii
    if (volt < 3.3f)
    {
      BEEP_Beep(1000, 100);
    }
    
    HAL_Delay(1000);
  }
}
```

## Typické hodnoty

### Svetlo (lux)
- **0-10:** Tma
- **10-100:** Slabé svetlo (súmrak)
- **100-500:** Bežná miestnosť
- **500-1000:** Dobre osvetlená miestnosť
- **1000+:** Jasné svetlo/vonku

### Batéria (V) - Li-Ion
- **4.2V:** Plne nabitá (100%)
- **3.7V:** Polovica (50%)
- **3.3V:** Nízka (25%)
- **3.0V:** Kritická (<10%)

## Kalibrácia

### Svetlo
1. Porovnajte s luxmetrom
2. Upravte `LUX_CONVERSION_FACTOR` v `analog.c`

### Batéria
1. Zmerajte rezistory R1 a R2 multiometrom
2. Aktualizujte `ANALOG_BAT_R1` a `ANALOG_BAT_R2` v `analog.h`

## Riešenie problémov

| Problém | Riešenie |
|---------|----------|
| Svetlo vždy 0 | Skontrolujte zapojenie fotodiody a TIA |
| Batéria vždy 0 | Skontrolujte zapojenie deliča na PB0 |
| Nesprávne hodnoty | Skalibrujte rezistory/conversion factor |

## Viac informácií
Pozri detailný návod: `ANALOG_README.md`
