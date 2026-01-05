# MIC (PDM) audio driver – 50 ms RMS (farebná hudba)

Tento modul číta PDM mikrofón cez **SPI1 + DMA** (HAL only) a robí jednoduchú PDM→PCM decimáciu.

## Cieľ
- update každých **50 ms** (20 Hz)
- výstup: **RMS** a **dBFS** z 50 ms okna
- určené pre „farebnú hudbu“ (VU meter)

## Ako to funguje (v skratke)
1. SPI1 v master RX-only móde generuje hodiny na **PA5 (SCK)**.
2. Mikrofón posiela 1-bitový PDM stream na **PA6 (MISO)**.
3. DMA zapisuje prijaté 16-bit slová do RAM.
4. Softvér spraví jednoduchú decimáciu: každé 16-bit slovo -> 1 sample (popcount - 8) / 8.
5. Z sample spočíta RMS a dBFS.

## API
- `MIC_Init()` – zavolaj raz po `MX_SPI1_Init()`.
- `MIC_Start()` / `MIC_Stop()` – spustené snímanie.
- `MIC_Task()` – volaj často v main loop.
- `MIC_GetLast50ms(&dbfs, &rms)` – získa posledný hotový výsledok.
- `MIC_SetDebug(1)` – zapne detailný UART debug (printf).

## Debug tipy
- CLOCK na PA5 uvidíš iba počas prenosu (keď SPI beží).
- Ak je timeout → najčastejšie nejde DMA IRQ.
- Ak DMA dobehne, ale buffer sa nezmení → nesedí DMA alignment/size alebo SPI DataSize.
