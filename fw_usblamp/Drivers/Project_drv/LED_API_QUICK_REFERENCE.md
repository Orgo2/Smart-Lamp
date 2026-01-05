# LED Driver API - Quick Reference

## Inicializácia

```c
// Inicializácia ovládača LED - automaticky zapne napájanie, počká 500ms (low power mode), prepne na 8MHz
LED_Init(&htim2, TIM_CHANNEL_1);
```

## Nastavenie farieb LED

### Pomocou parserom príkazov (cez USB/UART)
```c
// Formát: LED(čísla_LED)&FARBA(intenzita)
LED_ParseCommand("LED(1,5,10)&R(100)");     // LED 1,5,10 červená, intenzita 100
LED_ParseCommand("LED(2,8,15)&G(50)");      // LED 2,8,15 zelená, intenzita 50
LED_ParseCommand("LED(3,7,20)&B(200)");     // LED 3,7,20 modrá, intenzita 200
LED_ParseCommand("LED(4,12,25)&W(150)");    // LED 4,12,25 biela, intenzita 150
```

## Vypínanie LED

### Nová variadická funkcia LED_OFF()

```c
// Vypnutie všetkých LED (napájanie zostáva zapnuté)
LED_OFF(0);

// Vypnutie konkrétnych LED
LED_OFF(3, 5, 9, 10);        // Vypne LED 5, 9, 10 (prvý parameter = počet LED)
LED_OFF(1, 16);              // Vypne LED 16
LED_OFF(4, 1, 8, 9, 20);     // Vypne LED 1, 8, 9, 20
LED_OFF(2, 15, 22);          // Vypne LED 15, 22

// Cez parser
LED_ParseCommand("LED(OFF)");              // Vypne všetky LED
LED_ParseCommand("LED(1,8,9)&OFF");        // Vypne LED 1,8,9
```

## Deinicializácia (vypnutie celého systému)

```c
// Vypne všetky LED a vypne napájanie
LED_Deinit();
```

## Pokročilé funkcie

### Manuálna kontrola taktovacieho signálu
```c
LED_SetClockSpeed(8);    // 8MHz pre ovládanie LED
LED_SetClockSpeed(4);    // 4MHz pre nízku spotrebu v idle
LED_SetClockSpeed(16);   // 16MHz pre vyšší výkon (ak potrebné)
```

### Získanie stavu ovládača
```c
LED_State_t state = LED_GetState();
// LED_STATE_IDLE - pripravený na príkazy
// LED_STATE_BUSY - práve odosiela dáta
// LED_STATE_POWER_ON_DELAY - čaká na stabilizáciu napájania
// LED_STATE_ERROR - chyba
```

## Kompletný príklad

```c
// 1. Inicializácia
LED_Init(&htim2, TIM_CHANNEL_1);

// 2. Nastavenie LED na rôzne farby
LED_ParseCommand("LED(1,2,3)&R(255)");      // Prvé 3 LED červené
LED_ParseCommand("LED(4,5,6)&G(255)");      // Ďalšie 3 LED zelené  
LED_ParseCommand("LED(7,8,9)&B(255)");      // Ďalšie 3 LED modré
LED_ParseCommand("LED(10,11,12)&W(255)");   // Ďalšie 3 LED biele

// 3. Vypnutie konkrétnych LED (ostatné zostanú zapnuté)
LED_OFF(3, 1, 2, 3);                        // Vypne prvé 3 LED (červené)

// 4. Opäť zapneme prvú LED, ale inou farbou
LED_ParseCommand("LED(1)&B(128)");          // LED 1 teraz modrá s intenzitou 128

// 5. Vypneme všetko
LED_OFF(0);                                 // Všetky LED vypnuté, napájanie zapnuté

// 6. Úplné vypnutie
LED_Deinit();                               // Vypne aj napájanie
```

## Príklad s dynamickou animáciou

```c
void LED_Rainbow_Example(void) {
    LED_Init(&htim2, TIM_CHANNEL_1);
    
    // Červená vlna
    for (int i = 1; i <= 30; i++) {
        char cmd[32];
        sprintf(cmd, "LED(%d)&R(%d)", i, i * 8);
        LED_ParseCommand(cmd);
        HAL_Delay(50);
    }
    
    // Prepínanie na zelenú
    for (int i = 1; i <= 30; i++) {
        char cmd[32];
        sprintf(cmd, "LED(%d)&G(200)", i);
        LED_ParseCommand(cmd);
        HAL_Delay(50);
    }
    
    // Postupné vypínanie
    for (int i = 1; i <= 30; i++) {
        LED_OFF(1, i);
        HAL_Delay(50);
    }
    
    LED_Deinit();
}
```

## Príklad s úsporou energie

```c
void LED_PowerSaving_Example(void) {
    // Inicializácia automaticky prepne na 8MHz
    LED_Init(&htim2, TIM_CHANNEL_1);
    
    // Rýchle operácie s LED
    LED_ParseCommand("LED(1,2,3,4,5)&W(255)");
    LED_ParseCommand("LED(10,11,12)&R(100)");
    
    // Prepnutie na nízku spotrebu keď čakáme na príkaz
    LED_SetClockSpeed(4);  // 4MHz = nízka spotreba
    
    // ... čakanie na USB príkaz alebo tlačidlo ...
    
    // Pred ďalšou operáciou prepneme späť na 8MHz
    LED_SetClockSpeed(8);
    LED_ParseCommand("LED(20,21,22)&B(150)");
    
    // Úplné vypnutie
    LED_Deinit();
}
```

## Dôležité poznámky

### Číslovanie LED
- LED sú číslované od **1 do 30** (nie 0 až 29)
- `LED_OFF(3, 5, 9, 10)` - prvý parameter (3) je počet LED, nasledujú čísla LED

### Pamäťový state
- Ovládač si pamätá stav všetkých 30 LED
- Keď nastavíte LED 5, ostatné LED zostanú v pôvodnom stave
- Vždy sa odosielajú dáta pre všetkých 30 LED (podľa SK6812 datasheetu)

### Farby
- **R** - červená (Red)
- **G** - zelená (Green)
- **B** - modrá (Blue)
- **W** - biela (White)
- Intenzita: 0-255

### Spotreba energie
| Režim | Frekvencia | Prúd |
|-------|------------|------|
| Inicializácia (sleep) | - | ~4 mA |
| Ovládanie LED | 8 MHz | ~8 mA |
| Idle | 4 MHz | ~3.5 mA |

### Časovanie @ 8MHz
- Perióda: 1.25µs (spec: 1.2µs ± tolerance) ✓
- 0-bit high: 375ns (spec: 300ns ± 150ns) ✓
- 1-bit high: 750ns (spec: 600ns ± 150ns) ✓

## Prehľad zmien oproti staršej verzii

| Stará funkcia | Nová funkcia | Poznámka |
|---------------|--------------|----------|
| `LED_PowerOn()` | - | Odstránené (merged do Init) |
| `LED_PowerOff()` | `LED_Deinit()` | Preimenované |
| `LED_AllOff()` | `LED_OFF(0)` | Nahradené variadickou funkciou |
| - | `LED_OFF(n, ...)` | Nová variadická funkcia |

## Návratové hodnoty

```c
HAL_StatusTypeDef status = LED_Init(&htim2, TIM_CHANNEL_1);
if (status != HAL_OK) {
    // Chyba inicializácie
}

// Možné hodnoty:
// HAL_OK - úspech
// HAL_ERROR - chyba (neplatný parameter, chyba nastavenia)
// HAL_BUSY - ovládač je zaneprázdnený (odosiela dáta)
```

## Troubleshooting

### LED sa nerozsviecia
1. Skontrolujte `CTL_LEN` pin (PB5) - musí byť nastavený ako OUTPUT
2. Overte `LED_Init()` úspešne prebehla
3. Skontrolujte pripojenie dátového pinu (PA8)

### Nesprávne farby
1. Overte poradie GRBW v datasheete vašich LED
2. Skontrolujte napájacie napätie (5V pre SK6812)

### Vysoká spotreba
1. Overte že `LED_SetClockSpeed(8)` sa volá
2. Skontrolujte že SLEEP mode sa používa počas init
3. Po LED operáciách prepnite na 4MHz pomocou `LED_SetClockSpeed(4)`
