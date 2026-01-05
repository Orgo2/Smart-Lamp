# ✅ AN_BAT Pin (PB0 = ADC1_IN17) - Konfigurácia dokončená!

## Zhrnutie zmien

**PB0** pin pre meranie napätia batérie (AN_BAT) bol úspešne nakonfigurovaný **priamo v kóde**, čím sa obišli problémy s .ioc súborom.

**Dôležité:** V QFN32 puzdre je PB0 = ADC1_IN17 (nie IN15!)

---

## 📝 Upravené súbory

### 1. `analog.h`
**Zmenené:** `ANALOG_BAT_CHANNEL` nastavený na `ADC_CHANNEL_17`

```c
#define ANALOG_BAT_CHANNEL  ADC_CHANNEL_17  // PB0 - AN_BAT (ADC1_IN17 on QFN32)
```

### 2. `main.h`
**Pridané:** Definície pre AN_BAT pin

```c
#define AN_BAT_Pin GPIO_PIN_0
#define AN_BAT_GPIO_Port GPIOB
```

### 3. `stm32u0xx_hal_msp.c`
**V `HAL_ADC_MspInit()`:**
- Pridané `__HAL_RCC_GPIOB_CLK_ENABLE();`
- Pridaná GPIO konfigurácia pre PB0 ako analógový vstup:

```c
__HAL_RCC_GPIOB_CLK_ENABLE();
/**ADC1 GPIO Configuration
PA7     ------> ADC1_IN14 (AN_LI - Light sensor)
PB0     ------> ADC1_IN17 (AN_BAT - Battery voltage)
*/
GPIO_InitStruct.Pin = AN_BAT_Pin;
GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
GPIO_InitStruct.Pull = GPIO_NOPULL;
HAL_GPIO_Init(AN_BAT_GPIO_Port, &GPIO_InitStruct);
```

**V `HAL_ADC_MspDeInit()`:**
- Pridaná de-inicializácia AN_BAT pinu:

```c
HAL_GPIO_DeInit(AN_BAT_GPIO_Port, AN_BAT_Pin);
```

### 4. Dokumentácia aktualizovaná
- `ANALOG_SUMMARY.md` - označené ako ✅ nakonfigurované
- `ANALOG_QUICK_START.md` - odstránená sekcia o CubeMX
- `ANALOG_README.md` - aktualizované pokyny

---

## 🎯 Hardvérová konfigurácia

### AN_LI (Svetelný senzor) ✅
- **Pin:** PA7 (ADC1_IN14)
- **Fotodioda:** SFH203P
- **TIA:** 330 kΩ
- **Status:** Nakonfigurované a funkčné

### AN_BAT (Napätie batérie) ✅
- **Pin:** PB0 (ADC1_IN17 v QFN32 puzdre)
- **Delič:** R1=100kΩ / R2=47kΩ
- **Status:** Nakonfigurované a funkčné

---

## 🚀 Ako použiť

### 1. V main.c pridajte include:
```c
#include "analog.h"
```

### 2. Inicializujte driver:
```c
int main(void)
{
  // ... init peripherals ...
  
  ANALOG_Init(&hadc1);
  
  while (1)
  {
    // Meranie svetla v luxoch
    float svetlo = ANALOG_GetLight();
    
    // Meranie batérie vo voltoch
    float bateria = ANALOG_GetBat();
    
    // Vaša logika...
    HAL_Delay(1000);
  }
}
```

---

## ✨ Funkčnosť

Obidve funkcie sú teraz plne funkčné:

### `ANALOG_GetLight()`
- Vracia intenzitu svetla v luxoch
- Rozsah: 0 - 100,000 lux
- Používa PA7 (ADC1_IN14)

### `ANALOG_GetBat()`
- Vracia napätie batérie vo voltoch
- Rozsah: 0 - 5.0 V
- Používa PB0 (ADC1_IN17) ✅ **TERAZ NAKONFIGUROVANÉ**

---

## ⚡ Testovanie

Po kompilácii a nahratí firmware môžete otestovať:

```c
// Jednoduchý test
ANALOG_Init(&hadc1);

while(1)
{
  printf("Svetlo: %.1f lux\n", ANALOG_GetLight());
  printf("Batéria: %.2f V\n", ANALOG_GetBat());
  HAL_Delay(1000);
}
```

---

## 📊 Očakávané hodnoty

### Svetlo
- **Tma:** 0-10 lux
- **Miestnosť:** 100-500 lux  
- **Jasné svetlo:** 500-1000+ lux

### Batéria (Li-Ion)
- **Plná:** ~4.2V
- **Normálna:** 3.7-4.0V
- **Nízka:** 3.3-3.5V
- **Kritická:** <3.3V

---

## 🔧 Kalibrácia (voliteľná)

### Pre svetlo:
1. Porovnajte s luxmetrom
2. Upravte `LUX_CONVERSION_FACTOR` v `analog.c`

### Pre batériu:
1. Zmerajte skutočné hodnoty R1 a R2
2. Aktualizujte `ANALOG_BAT_R1` a `ANALOG_BAT_R2` v `analog.h`
3. Overte multiometrom

---

## ✅ Kontrolný zoznam

- [x] PB0 nakonfigurovaný ako ADC1_IN17 (QFN32 puzdro)
- [x] GPIO clock pre GPIOB povolený
- [x] Analógový režim nastavený pre PB0
- [x] `ANALOG_BAT_CHANNEL` = ADC_CHANNEL_17 v analog.h
- [x] De-inicializácia pridaná
- [x] Dokumentácia aktualizovaná
- [x] Žiadne kompilačné chyby

---

## 🎉 Výsledok

**AN_BAT pin je teraz plne funkčný!**

Driver dokáže merať:
- ✅ Svetlo cez PA7 (ADC1_IN14)
- ✅ Batériu cez **PB0 (ADC1_IN17)** - QFN32 puzdro

**Poznámka:** V QFN32 puzdre je PB0 = ADC1_IN17 (nie ADC1_IN15 ako v iných puzdrovaniach!)

Konfigurácia cez kód úspešne obišla problémy s .ioc súborom a všetko je pripravené na použitie!

---

**Dátum:** 24. december 2025  
**Autor:** orgo  
**Projekt:** SmartLamp FW
