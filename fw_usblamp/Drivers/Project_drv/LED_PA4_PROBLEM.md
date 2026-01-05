Problém: SK6812 LED strip na PA4 (LPTIM2) bez DMA

PA4 má len LPTIM2 alternate function, nie TIM2.
LPTIM nemá DMA support → nemôžeme robiť presné SK6812 timing.

## Riešenie 1: GPIO Bit-banging s TIM2+DMA (ODPORÚČANÉ)

TIM2 Update Event → DMA → GPIO BSRR register (PA4)
- Presné časovanie
- DMA robí prácu, nie CPU
- Funguje aj keď pin nemá TIM AF

Potrebné:
1. TIM2 - Update event každých 1.25us (800kHz)
2. DMA - TIM2_UP → GPIOA->BSRR
3. Buffer s GPIO set/reset príkazmi

## Riešenie 2: LPTIM2 s prerušeniami (POMALÉ)

LPTIM2 prerušenie každý bit → manuálne nastavenie GPIO
- Nepresné (overhead prerušenia ~5-10us)
- Vysoké zaťaženie CPU
- Pravdepodobne NEBUDE fungovať pre SK6812

## Riešenie 3: Hardware prerobenie (NAJLEPŠIE)

Prepájať PA8 (TIM2 CH1) na LED strip namiesto PA4.
PA4 nepotrebuješ ak máš PA8 dostupný.

## Čo odporúčam?

**Prepoj LED strip dátový pin z PA4 na PA8!**
PA8 má TIM2 CH1 s DMA → existujúci kód bude fungovať perfektne.

Ak nemôžeš prepojiť hardware, musím implementovať GPIO bit-banging s TIM2+DMA.
