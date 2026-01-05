# BME280

## API

```c
BME280_Init(&hi2c1);

float val;
RH(&val);    // vlhkost %
T(&val);     // teplota °C
P(&val);     // tlak hPa

BME280_Data_t data;
BME280(&data);  // vsetko naraz

BME280_Deinit();
```

- CSB pin na VCC = adresa 0x76
- Timeout 5s - po 5s bez komunikacie vrati HAL_TIMEOUT
- Max presnost: oversampling x16, filter x16