#pragma once
#include <Arduino.h>

// ---------------------------------------------------------------------------
// Распиновка. GC9A01 — SPI, только запись, линия MISO к дисплею не идёт.
//
// Но PIN_MISO всё равно приходится задавать, и это не формальность.
// SPIClass::begin() при miso = -1 НЕ оставляет линию в покое, а подставляет
// штатный пин шины и делает ему pinMode(INPUT): на ESP32-S3 для FSPI это
// GPIO13, на классическом ESP32 для VSPI — GPIO19. Если такой пин занят под
// DC или RST, ядро молча превратит его во вход уже после нашего pinMode(),
// и дисплей перестанет различать команды и данные — подсветка горит,
// экран пустой. Поэтому сюда ставится заведомо свободный пин.
// ---------------------------------------------------------------------------
// Arduino IDE не умеет передавать -DBOARD_... из настроек проекта: платы там
// выбираются мышкой, а не флагами компилятора. Поэтому если рядом лежит
// board_select.h — берём плату и флаги оттуда. В PlatformIO этого файла нет,
// и всё решают build_flags. Скетч для Arduino IDE собирает tools/make_arduino_sketch.sh.
#if __has_include("board_select.h")
  #include "board_select.h"
#endif

#if defined(BOARD_S3_N16R8)
  // ESP32-S3-DevKitC-1 N16R8. SCK/MOSI/CS посажены на пины, которые идут
  // в SPI2 напрямую через IO MUX, — только так шина держит 80 МГц.
  // Занято на этой плате и трогать нельзя:
  //   GPIO26..32 — флеш, GPIO33..37 — октальная PSRAM (у N16R8 она OPI),
  //   GPIO0, 3, 45, 46 — strapping, GPIO19, 20 — USB, GPIO43, 44 — UART0.
  #define PIN_SCK   12
  #define PIN_MOSI  11
  #define PIN_CS    10
  #define PIN_DC    13
  #define PIN_RST   14
  #define PIN_BL    21     // подсветка, -1 если на плате дисплея посажена на 3V3
  #define PIN_MISO   9     // не подключён к дисплею, см. пояснение ниже
#elif defined(BOARD_S3_YASMINA)
  // Плата Сергея — та, что уже спаяна (ESP32-S3 N16R8, репозиторий yasminalia).
  // Распиновка здесь — ЕГО, менять нельзя: провода уже в плате.
  // Программная часть при этом наша: свой драйвер GC9A01 вместо Adafruit_ST7789.
  #define PIN_SCK    9
  #define PIN_MOSI   8
  #define PIN_CS    10
  #define PIN_DC    11
  #define PIN_RST   12
  #define PIN_BL    -1     // BLK у него на 3V3, отдельного пина нет
  #define PIN_MISO  18     // к дисплею не идёт, просто свободный пин (см. выше)
#elif defined(BOARD_S3_SUPERMINI)
  #define PIN_SCK   4
  #define PIN_MOSI  6
  #define PIN_DC    5
  #define PIN_CS    7
  #define PIN_RST   8
  #define PIN_BL    9      // подсветка, -1 если посажена на 3V3
  #define PIN_MISO  10     // не подключён к дисплею, см. пояснение ниже
#elif defined(BOARD_ESP32_WROOM)
  #define PIN_SCK   18
  #define PIN_MOSI  23
  #define PIN_DC    2
  #define PIN_CS    5
  #define PIN_RST   4
  #define PIN_BL    15
  #define PIN_MISO  19     // не подключён к дисплею, см. пояснение ниже
#else
  #error "Не выбрана плата: задай BOARD_S3_YASMINA, BOARD_S3_N16R8, BOARD_S3_SUPERMINI или BOARD_ESP32_WROOM"
#endif

// Тактовая для ПИКСЕЛЕЙ. 40 МГц работает на обычных проводах; 80 МГц даёт
// 50 кадров вместо 38, но на длинных перемычках срывается — поднимай, только
// если убедился, что картинка чистая и панель стартует каждый раз.
#ifdef PANEL_TEST_SLOW_SPI
  #define SPI_HZ      10000000
#else
  #define SPI_HZ      40000000
#endif

// Тактовая для ИНИЦИАЛИЗАЦИИ панели — всегда медленная и всегда безопасная.
// Это несколько десятков байт один раз за запуск, их скорость не значит
// ничего. Зато если хоть один байт таблицы не дойдёт, контроллер не
// проснётся: экран останется чёрным, а прошивка будет исправно работать
// и рапортовать в порт. Симптом узнаётся по тому, что после очередной
// перезагрузки панель вдруг заводится — инициализация на грани срыва.
#define SPI_HZ_INIT   10000000

// ---------------------------------------------------------------------------
// Геометрия панели
// ---------------------------------------------------------------------------
#define TFT_W         240
#define TFT_H         240

// Лицо живёт только в этой полосе экрана. Остальное — статичный чёрный фон,
// который заливается один раз при старте. Так на кадр уходит вдвое меньше SPI.
#define FACE_Y0       56
#define FACE_Y1       196

// Высота полосы рендера. 240*BAND_H*2 байт держится в стеке задачи.
#define BAND_H        24

// MADCTL: поворот/зеркало/порядок цветов. 0x08 = BGR без поворота.
// Если картинка зеркальная — попробуй 0x48, 0x88 или 0xC8.
#define MADCTL        0x08

// ---------------------------------------------------------------------------
// Голосовая часть. Распиновка микрофона, усилителя и кнопки взята как есть
// из платы Сергея (github.com/sergey2a/yasminalia) — там уже всё припаяно.
// ---------------------------------------------------------------------------
#ifdef BOARD_S3_YASMINA

// Сенсорная кнопка TTP223: касание = HIGH.
#define PIN_TOUCH        4

// INMP441 — I2S RX, порт 0.
#define PIN_MIC_SCK      5
#define PIN_MIC_WS       6
#define PIN_MIC_SD       7

// MAX98357 — I2S TX, порт 1.
#define PIN_SPK_BCLK    15
#define PIN_SPK_LRC     16
#define PIN_SPK_DIN     17

#define AUDIO_SAMPLE_RATE  16000
#define AUDIO_MAX_SEC      20      // потолок одной реплики

// Сервер диалога. Тот же, что у Сергея.
#define API_HOST         "https://tatarser.213-109-202-195.sslip.io"
#define API_PATH         "/v1/dialog/audio"
#define API_SPEAKER      "alsu"
#define API_FORMAT       "wav"

// Wi-Fi. Свои логин и пароль клади в include/secrets.h — он в .gitignore:
//     #define WIFI_SSID "..."
//     #define WIFI_PASS "..."
#if __has_include("secrets.h")
  #include "secrets.h"
#endif
#ifndef WIFI_SSID
  #define WIFI_SSID  "WiFi SSID"
  #define WIFI_PASS  "PASSWORD"
#endif

#endif // BOARD_S3_YASMINA
