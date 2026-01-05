# ANALOG Driver - Zhrnutie implementácie

## ✅ Hotové

Driver pre analógové merania pre SmartLamp projekt bol úspešne vytvorený!

## 📁 Vytvorené súbory

### Zdrojové súbory
1. **`analog.h`** - Header súbor s definíciami a API
2. **`analog.c`** - Implementácia drivera
3. **`analog_example.c`** - Príklady použitia

### Dokumentácia
4. **`ANALOG_README.md`** - Kompletná dokumentácia (slovensky)
5. **`ANALOG_QUICK_START.md`** - Rýchly návod na použitie

## 🔧 Implementované funkcie

### 1. ANALOG_Init(ADC_HandleTypeDef *hadc)
- Inicializuje driver
- Kalibruje ADC pre presné merania
- Volať raz pri štarte programu

### 2. ANALOG_GetLight(void)
- **Vracia:** Intenzita svetla v luxoch (lx)
- **Princíp:** 
  - Fotodioda SFH203P generuje prúd úmerný svetlu
  - TIA (330kΩ) konvertuje prúd na napätie
  - ADC číta napätie a prepočíta na luxy
- **Rozsah:** 0 - 100,000 lux
- **Pin:** PA7 (ADC1_IN14) - AN_LI ✓ nakonfigurované

### 3. ANALOG_GetBat(void)
- **Vracia:** Napätie batérie vo voltoch (V)
- **Princíp:**
  - Rezistorový delič (100kΩ / 47kΩ) znižuje napätie
  - ADC číta znížené napätie
  - Prepočet na skutočné napätie batérie
- **Rozsah:** 0 - 5.0 V
- **Pin:** PB0 (ADC1_IN15) - AN_BAT ✅ nakonfigurované

## ⚙️ Technické detaily

### ADC konfigurácia (už nastavené)
- Rozlíšenie: 12-bit s 16× oversamplingom (efektívne 16-bit)
- Referenčné napätie: 3.3V
- Vzorkovanie: 160.5 cyklov
- Priemerné vzorky: 10 na meranie

### Hardware požiadavky

#### Svetelný senzor (AN_LI) ✓
- **Fotodioda:** SFH203P (Silicon PIN photodiode)
- **Responzivita:** ~0.35 A/W (viditeľné svetlo)
- **Aktívna plocha:** 7.45 mm²
- **TIA rezistor:** 330 kΩ
- **Pin:** PA7 (ADC1_IN14) - už nakonfigurované

#### Meranie batérie (AN_BAT) ✅
- **Rezistory:**
  - R1 (horný): 100 kΩ
  - R2 (dolný): 47 kΩ
- **Delič:** 0.3197 (47k / 147k)
- **Max vstup:** ~5V batéria → ~1.6V na ADC
- **Pin:** PB0 (ADC1_IN15) - nakonfigurované v kóde

## 📋 Konfigurácia hotová!

### ✅ AN_BAT pin nakonfigurovaný v kóde

AN_BAT pin (PB0 - ADC1_IN15) je **nakonfigurovaný priamo v kóde** v súboroch:

- **main.h**: Pridaná definícia `AN_BAT_Pin` a `AN_BAT_GPIO_Port`
- **stm32u0xx_hal_msp.c**: Pridaná GPIO konfigurácia v `HAL_ADC_MspInit()`
- **analog.h**: Nastavený `ANALOG_BAT_CHANNEL` na `ADC_CHANNEL_15`

Táto konfigurácia obchádza problémy s .ioc súborom a je plne funkčná!

## 🚀 Rýchle použitie

### V main.c pridajte:

```c
#include "analog.h"

int main(void)
{
  // ... HAL init, peripherals init ...
  
  // Inicializovať analog driver
  ANALOG_Init(&hadc1);
  
  while (1)
  {
    // Meranie svetla
    float lux = ANALOG_GetLight();
    
    // Meranie batérie
    float volt = ANALOG_GetBat();
    
    // Vaša logika...
    if (lux < 100.0f)
    {
      // Je tma, zapnúť LED
    }
    
    if (volt < 3.3f)
    {
      // Nízka batéria, upozorniť
    }
    
    HAL_Delay(1000);
  }
}
```

## 📊 Kalibrácia

### Pre presné meranie svetla:
1. Použite luxmeter pre referenčné meranie
2. Porovnajte s hodnotami z `ANALOG_GetLight()`
3. Upravte `LUX_CONVERSION_FACTOR` v `analog.c`

### Pre presné meranie batérie:
1. Zmerajte skutočné hodnoty rezistorov R1 a R2
2. Aktualizujte `ANALOG_BAT_R1` a `ANALOG_BAT_R2` v `analog.h`
3. Overte multiometrom

## 💡 Príklady typických hodnôt

### Svetlo
- **Tma:** < 10 lux
- **Súmrak:** 10-100 lux
- **Miestnosť:** 100-500 lux
- **Jasné svetlo:** 500-1000 lux
- **Vonku/slnko:** > 1000 lux

### Batéria (Li-Ion)
- **Plne nabitá:** 4.2V (100%)
- **Dobrý stav:** 3.7-4.0V (50-80%)
- **Nízka:** 3.3-3.6V (20-50%)
- **Kritická:** < 3.3V (< 20%)
- **Vybitá:** < 3.0V

## 📚 Dokumentácia

- **ANALOG_README.md** - Podrobná dokumentácia
- **ANALOG_QUICK_START.md** - Rýchly štart
- **analog_example.c** - Príklady kódu

## 🔍 Testovanie

Po nakonfigurovaní AN_BAT môžete použiť funkcie z `analog_example.c`:
- `ANALOG_SimpleTest()` - Jednoduchý test výpisu
- `ANALOG_Example()` - Kompletný príklad s klasifikáciou
- `ANALOG_LightCalibration()` - Kalibrácia svetla
- `ANALOG_BatteryCalibration()` - Kalibrácia batérie
- `ANALOG_DataLogger()` - CSV logging do USB

## ⚠️ Poznámky

1. **AN_BAT pin je nakonfigurovaný** - PB0 (ADC1_IN15) je ready to use ✅
2. **TIA obvod** musí byť správne zapojený pre `ANALOG_GetLight()`
3. **Rezistorový delič** pre batériu by mal používať presné rezistory (±1%)
4. **Kalibrácia** zlepší presnosť merania

## 📞 Podpora

Ak máte problémy:
1. Skontrolujte hardvérové zapojenie
2. Overte konfiguráciu pinov v CubeMX
3. Pozrite sekciu "Riešenie problémov" v ANALOG_README.md

## ✨ Hotovo!

Driver je pripravený na použitie. Môžete merať:
- ✅ Intenzitu svetla v luxoch (PA7 - ADC1_IN14)
- ✅ Napätie batérie vo voltoch (PB0 - ADC1_IN15)

Všetky piny sú nakonfigurované v kóde a driver je plne funkčný!

Šťastné kódovanie! 🎉

---
**Vytvorené:** 24. december 2025  
**Autor:** orgo  
**Projekt:** SmartLamp FW
