Ясмина — скетч для Arduino IDE
==============================

Папка собрана скриптом tools/make_arduino_sketch.sh, руками её не правят:
правки идут в src/ основного проекта, потом скрипт прогоняется заново.

1. Плата
   Boards Manager → "esp32" by Espressif Systems, версия 3.x.
   Выбрать: Tools → Board → ESP32 Arduino → ESP32S3 Dev Module.

2. Настройки платы (Tools) — важны все четыре:
   PSRAM ................... OPI PSRAM          ← без этого не выделится запись
   Flash Size .............. 16MB (128Mb)
   Partition Scheme ........ 16M Flash (3MB APP/9.9MB FATFS)
   USB CDC On Boot ......... Disabled           ← Serial уходит в UART0, на CH343

3. Библиотека
   Sketch → Include Library → Manage Libraries → "ArduinoJson" (Benoit Blanchon), 7.x.
   Больше ничего ставить не нужно: драйвер дисплея и лицо свои,
   Adafruit_GFX и Adafruit_ST7789 здесь не используются.

4. Wi-Fi
   Открыть вкладку secrets.h и вписать свои SSID и пароль.

5. Залить и открыть Serial Monitor на 115200.
   Работают команды: 1..9 — эмоция, t — включить/выключить речь.

Распиновка не менялась — она из платы, которая уже спаяна:
  дисплей  SCLK 9  MOSI 8  CS 10  DC 11  RST 12   (подсветка на 3V3)
  микрофон SCK  5  WS   6  SD  7                  (I2S0, INMP441)
  динамик  BCLK 15 LRC  16 DIN 17                 (I2S1, MAX98357)
  кнопка   TOUCH 4                                (TTP223, касание = HIGH)
