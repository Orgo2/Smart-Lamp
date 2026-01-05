# BEEP Driver

Zdieľa TIM2 s LED driverom.

## API

```c
// Init - TIM2 použitý aj pre LED (CH1) aj BEEP (CH2)
BEEP_Init(&htim2, TIM_CHANNEL_2, TIM_CHANNEL_1);  // beep CH2, LED CH1

// Pípnutie
BEEP(1000, 100, 1);    // 1kHz, 100% hlasitosť, 1s

// Stop
BEEP_Stop();
```

## Zdieľanie TIM2

- LED používa TIM2 CH1 (PA8) + DMA
- BEEP používa TIM2 CH2 (PA15)
- BEEP čaká kým LED ukončí prenos (max 1s timeout)
- Po BEEP sa LED môže opäť použiť

## Použitie

```c
LED_Init(&htim2, TIM_CHANNEL_1);
BEEP_Init(&htim2, TIM_CHANNEL_2, TIM_CHANNEL_1);

LED_ParseCommand("LED(1,2,3)&R(255)");  // LED pošle dáta
// LED_STATE_BUSY počas DMA

BEEP(440, 50, 0.5);  // Počká na LED_STATE_IDLE, potom pípa
```
