# Drivers

## LED - LPTIM2 @ 24MHz s DMA (PA4)
```c
LED_Init(&hlptim2, LPTIM_CHANNEL_1);
LED_ParseCommand("LED(1,5,10)&R(100)");
LED_OFF(3, 1, 5, 10);
LED_Deinit();
```
**CubeMX:** LPTIM2 PWM CH1 + DMA (LPTIM2_IC1 → Memory to Peripheral)

**24MHz timing:**
- 0 bit: 292ns H + 875ns L (spec: 300ns ±150 / 900ns) ✓
- 1 bit: 583ns H + 583ns L (spec: 600ns ±150 / 600ns) ✓

**Výhody DMA:**
- Žiadne prerušenia počas prenosu
- Presné časovanie bez jitteru
- ~1.25ms prenos 30 LED bez CPU load

## BEEP - LPTIM3 (PA15)
```c
BEEP_Init(&hlptim3, LPTIM_CHANNEL_3);
BEEP(1000, 100, 1);  // 1kHz, 100%, 1s
BEEP_Stop();
```

## BME280 - I2C1
```c
BME280_Init(&hi2c1);
float val;
RH(&val);
T(&val);
P(&val);
BME280_Deinit();
```