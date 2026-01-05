# ANALOG Driver - SmartLamp Project

## Prehľad
Driver pre analógové merania pre projekt inteligentnej lampy. Podporuje meranie intenzity osvetlenia pomocou fotodiody a transimpedančného zosilňovača (TIA) a meranie napätia batérie pomocou rezistorového deliča.

## Funkcie

### 1. ANALOG_Init()
**Popis:** Inicializuje analógový driver a kalibruje ADC.

**Syntax:**
```c
void ANALOG_Init(ADC_HandleTypeDef *hadc);
```

**Parametre:**
- `hadc`: Pointer na ADC handle (typicky `&hadc1`)

**Príklad použitia:**
```c
ANALOG_Init(&hadc1);
```

### 2. ANALOG_GetLight()
**Popis:** Meria intenzitu osvetlenia v luxoch pomocou TIA a fotodiody SFH203P.

**Syntax:**
```c
float ANALOG_GetLight(void);
```

**Návratová hodnota:** Intenzita svetla v luxoch (lx)
- Rozsah: 0 - 100,000 lux
- Rozlíšenie: závisí od ADC rozlíšenia (16-bit efektívne s oversamplingom)

**Príklad použitia:**
```c
float light_level = ANALOG_GetLight();
printf("Svetlo: %.1f lux\n", light_level);
```

### 3. ANALOG_GetBat()
**Popis:** Meria napätie batérie vo voltoch pomocou rezistorového deliča.

**Syntax:**
```c
float ANALOG_GetBat(void);
```

**Návratová hodnota:** Napätie batérie vo voltoch (V)
- Rozsah: 0 - 5.0 V
- Rozlíšenie: ~76 µV (teoreticky s 16-bit ADC)

**Príklad použitia:**
```c
float battery_voltage = ANALOG_GetBat();
printf("Batéria: %.2f V\n", battery_voltage);
```

## Hardvérová konfigurácia

### Light Sensor (Svetelný senzor)
- **Fotodioda:** SFH203P (Silicon PIN photodiode)
- **TIA rezistor:** 330 kΩ
- **ADC pin:** PA7 (ADC1_IN14) - označený ako AN_LI
- **Princíp:**
  - Fotodioda generuje prúd úmerný intenzite svetla
  - TIA (Trans-Impedance Amplifier) konvertuje prúd na napätie: V_out = I_photo × R_TIA
  - ADC číta výstupné napätie a prepočítava na luxy

### Battery Voltage (Napätie batérie)
- **Rezistorový delič:**
  - R1 (horný): 100 kΩ
  - R2 (dolný): 47 kΩ
  - Delič: 47k / (100k + 47k) = 0.3197
- **ADC pin:** PB0 (ADC1_IN15) - AN_BAT ✅ nakonfigurované v kóde
- **Max. vstupné napätie:** ~5.0 V (batéria)
- **Max. napätie na ADC:** ~1.6 V (po deliči)

### ADC konfigurácia
- **Rozlíšenie:** 12-bit (4096 úrovní)
- **Oversampling:** 16× (efektívne 16-bit, 65536 úrovní)
- **Referenčné napätie:** 3.3 V
- **Vzorkovacia frekvencia:** 160.5 cyklov
- **Počet vzoriek na meranie:** 10 (priemerovanie)

## Kalibrácia a presnosť

### Light Sensor (Svetelný senzor)
**Charakteristiky SFH203P:**
- Responzivita @ 850nm: ~0.55 A/W
- Responzivita @ 555nm (viditeľné svetlo): ~0.35 A/W
- Aktívna plocha: 7.45 mm²

**Konverzný vzorec:**
```
I_photo = V_out / R_TIA
P_optical = I_photo / Responsivity
Irradiance = P_optical / Area
Lux = Irradiance × 683 × Responsivity
```

**Typické hodnoty:**
- Miestnosť s umelým osvetlením: 300-500 lux
- Kancelária: 400-1000 lux
- Priame slnečné svetlo: 32,000-100,000 lux
- Mesačné svetlo: ~1 lux

### Battery Voltage (Napätie batérie)
**Konverzný vzorec:**
```
V_adc = ADC_value × (3.3V / 65535)
V_bat = V_adc / 0.3197
```

**Presnosť:**
- ADC rozlíšenie: ~50 µV na ADC vstupe
- Rozlíšenie batérie: ~156 µV (po prepočte)
- Typická presnosť: ±1% (závisí od tolerancie rezistorov)

## Príklad kompletného použitia

```c
#include "analog.h"

int main(void)
{
    // Inicializácia hardvéru
    HAL_Init();
    SystemClock_Config();
    MX_GPIO_Init();
    MX_ADC1_Init();
    
    // Inicializácia analog drivera
    ANALOG_Init(&hadc1);
    
    while (1)
    {
        // Meranie svetla
        float light = ANALOG_GetLight();
        printf("Intenzita svetla: %.1f lux\n", light);
        
        // Meranie napätia batérie
        float battery = ANALOG_GetBat();
        printf("Napätie batérie: %.2f V\n", battery);
        
        // Rozhodovanie na základe svetla
        if (light < 100.0f)
        {
            // Je tma, zapnúť LED
            LED_SetBrightness(100);
        }
        else if (light > 500.0f)
        {
            // Je dosť svetlo, vypnúť LED
            LED_SetBrightness(0);
        }
        
        // Upozornenie pri nízkej batérii
        if (battery < 3.3f)
        {
            printf("VAROVANIE: Nízka batéria!\n");
        }
        
        HAL_Delay(1000);
    }
}
```

## Poznámky a odporúčania

### Konfigurácia AN_BAT
- Pin PB0 (ADC1_IN15) je nakonfigurovaný priamo v kóde
- Nie je potrebná konfigurácia v CubeMX .ioc súbore
- Konfigurácia je v súboroch: main.h, stm32u0xx_hal_msp.c, analog.h

### Pre presné meranie svetla:
- Umiestnite fotodiodu tak, aby nebola zakrytá
- Chráňte pred priamym slnečným svetlom (môže presaturovať)
- Kalibrácia: porovnajte s luxmetrom a upravte `LUX_CONVERSION_FACTOR`

### Pre presné meranie batérie:
- Použite rezistory s toleranciou ±1% alebo lepšie
- Skalibrujte delič multiometrom a upravte `ANALOG_BAT_R1` a `ANALOG_BAT_R2`
- Pre Li-Ion batérie: plne nabitá ~4.2V, vybitá ~3.0V

### Optimalizácia spotreby energie:
- ADC má režim "LowPowerAutoPowerOff" povolený
- Medzi meraniami ADC automaticky vypne
- Pre ešte nižšiu spotrebu zvážte dlhšie intervaly medzi meraniami

### Filtrovanie šumu:
- Driver priemieruje 10 vzoriek na meranie
- Pre ešte stabilnejšie výsledky zvýšte `ANALOG_NUM_SAMPLES`
- Kompromis: vyššia presnosť vs. rýchlosť merania

## Riešenie problémov

### Svetlo vždy ukazuje 0 lux:
- Skontrolujte zapojenie fotodiody (katóda na zem, anóda na TIA)
- Overte, že PA7 je nakonfigurovaný ako ADC1_IN14
- Skontrolujte TIA obvod

### Svetlo ukazuje nereálne vysoké hodnoty:
- Upravte `LUX_CONVERSION_FACTOR` v analog.c
- Skontrolujte hodnotu TIA rezistora

### Napätie batérie vždy 0V:
- Skontrolujte zapojenie rezistorového deliča na PB0
- Overte, že rezistorový delič je pripojený medzi batériou a zemou
- Skontrolujte, či je PB0 spojený so stredom deliča (medzi R1 a R2)

### Napätie batérie ukazuje nesprávne hodnoty:
- Zmerajte skutočné hodnoty rezistorov R1 a R2
- Aktualizujte `ANALOG_BAT_R1` a `ANALOG_BAT_R2` v analog.h
- Overte referenčné napätie ADC (3.3V)

## Závislosť
- STM32 HAL Library
- `stm32u0xx_hal_adc.h` a `stm32u0xx_hal_adc_ex.h`
- `main.h` (pre Error_Handler)

## Autor
orgo - December 13, 2025

## Licencia
Súčasť SmartLamp projektu
