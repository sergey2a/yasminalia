#!/usr/bin/env bash
# Собирает папку скетча для Arduino IDE из этого проекта.
#
# Исходник один и тот же: скрипт не переписывает код, а раскладывает его так,
# как хочет Arduino IDE — всё одним слоем в папке с именем скетча, main.cpp
# становится .ino, а флаги из platformio.ini превращаются в board_select.h
# (см. пояснение в include/config.h).
#
#     tools/make_arduino_sketch.sh            → arduino/Yasmina/
#     tools/make_arduino_sketch.sh ~/Arduino  → ~/Arduino/Yasmina/
#
# После правок в src/ прогнать заново — папка пересоздаётся целиком.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="${1:-$root/arduino}/Yasmina"

rm -rf "$out"
mkdir -p "$out"

# Всё, кроме main.cpp (он станет скетчем) и panel_test.cpp (своя среда).
for f in "$root"/src/*.h "$root"/src/*.cpp; do
  case "$(basename "$f")" in
    main.cpp|panel_test.cpp) continue ;;
  esac
  cp "$f" "$out/"
done
cp "$root/include/config.h" "$out/"
cp "$root/src/main.cpp" "$out/Yasmina.ino"

cat > "$out/board_select.h" <<'HDR'
// Плата и режим сборки для Arduino IDE. В PlatformIO этого файла нет —
// там то же самое лежит в build_flags (среда yasmina).
#pragma once

#define BOARD_S3_YASMINA   // распиновка спаянной платы, см. config.h
#define BOARD_HAS_PSRAM    // N16R8, октальная PSRAM
#define VOICE_ENABLED      // голосовой диалог; убери — останется демо лица
HDR

# Wi-Fi. Кладём ТОЛЬКО заготовку, даже если рядом лежит заполненный
# include/secrets.h: папка скетча уезжает к инженеру и в git, и настоящий
# пароль в ней рано или поздно окажется в публичном репозитории.
cp "$root/include/secrets.h.example" "$out/secrets.h"

cat > "$out/README.txt" <<'TXT'
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
TXT

echo "скетч готов: $out"
ls "$out"
