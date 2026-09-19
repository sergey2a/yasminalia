#include "gc9a01.h"
#include "config.h"
#include <SPI.h>

namespace gc9a01 {

static SPIClass *bus = nullptr;
static SPISettings settings(SPI_HZ_INIT, MSBFIRST, SPI_MODE0);

static inline void csLow()  { digitalWrite(PIN_CS, LOW); }
static inline void csHigh() { digitalWrite(PIN_CS, HIGH); }
static inline void dcCmd()  { digitalWrite(PIN_DC, LOW); }
static inline void dcData() { digitalWrite(PIN_DC, HIGH); }

void setClock(uint32_t hz);

static void writeCmd(uint8_t c) {
  dcCmd();
  csLow();
  bus->write(c);
  csHigh();
}

static void writeData(const uint8_t *d, size_t n) {
  if (!n) return;
  dcData();
  csLow();
  bus->writeBytes(d, n);
  csHigh();
}

// Таблица инициализации GC9A01 от производителя панели.
// Формат: <команда>, <кол-во байт данных | 0x80 = после команды пауза 120 мс>, <данные...>
static const uint8_t INIT_SEQ[] PROGMEM = {
  0xEF, 0,
  0xEB, 1, 0x14,
  0xFE, 0,                       // inter register enable 1
  0xEF, 0,                       // inter register enable 2
  0xEB, 1, 0x14,
  0x84, 1, 0x40,
  0x85, 1, 0xFF,
  0x86, 1, 0xFF,
  0x87, 1, 0xFF,
  0x88, 1, 0x0A,
  0x89, 1, 0x21,
  0x8A, 1, 0x00,
  0x8B, 1, 0x80,
  0x8C, 1, 0x01,
  0x8D, 1, 0x01,
  0x8E, 1, 0xFF,
  0x8F, 1, 0xFF,
  0xB6, 2, 0x00, 0x20,
  0x36, 1, MADCTL,               // ориентация / порядок цветов
  0x3A, 1, 0x05,                 // 16 бит на пиксель (RGB565)
  0x90, 4, 0x08, 0x08, 0x08, 0x08,
  0xBD, 1, 0x06,
  0xBC, 1, 0x00,
  0xFF, 3, 0x60, 0x01, 0x04,
  0xC3, 1, 0x13,                 // power control 2
  0xC4, 1, 0x13,                 // power control 3
  0xC9, 1, 0x22,
  0xBE, 1, 0x11,
  0xE1, 2, 0x10, 0x0E,
  0xDF, 3, 0x21, 0x0C, 0x02,
  0xF0, 6, 0x45, 0x09, 0x08, 0x08, 0x26, 0x2A,   // gamma 1
  0xF1, 6, 0x43, 0x70, 0x72, 0x36, 0x37, 0x6F,   // gamma 2
  0xF2, 6, 0x45, 0x09, 0x08, 0x08, 0x26, 0x2A,   // gamma 3
  0xF3, 6, 0x43, 0x70, 0x72, 0x36, 0x37, 0x6F,   // gamma 4
  0xED, 2, 0x1B, 0x0B,
  0xAE, 1, 0x77,
  0xCD, 1, 0x63,
  0x70, 9, 0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03,
  0xE8, 1, 0x34,
  0x62, 12, 0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70,
  0x63, 12, 0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70,
  0x64, 7, 0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07,
  0x66, 10, 0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00,
  0x67, 10, 0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98,
  0x74, 7, 0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00,
  0x98, 2, 0x3E, 0x07,
  0x35, 0,                       // tearing effect on
  0x21, 0,                       // инверсия — у GC9A01 это норма, без неё цвета негативные
  0x11, 0x80,                    // sleep out + пауза
  0x29, 0x80,                    // display on + пауза
  0x00, 0x00                     // конец таблицы: читается парой, как и всё остальное
};

void begin() {
  pinMode(PIN_CS, OUTPUT);
  pinMode(PIN_DC, OUTPUT);
  csHigh();
  dcData();

  if (PIN_RST >= 0) {
    pinMode(PIN_RST, OUTPUT);
    digitalWrite(PIN_RST, HIGH); delay(10);
    digitalWrite(PIN_RST, LOW);  delay(20);
    digitalWrite(PIN_RST, HIGH); delay(150);
  }

  // На S3 нужен именно SPI2 (FSPI) — к нему подведены пины IO MUX 11/12/13.
  // На классическом ESP32 то же самое даёт VSPI с пинами 18/23/5.
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32C3
  bus = new SPIClass(FSPI);
#else
  bus = new SPIClass(VSPI);
#endif
  bus->begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);

  // begin() выше сам дёргает pinMode() для линий шины и мог перебить наши
  // настройки, поэтому управляющие пины назначаем ещё раз — уже наверняка.
  pinMode(PIN_CS, OUTPUT);
  pinMode(PIN_DC, OUTPUT);
  csHigh();
  bus->beginTransaction(settings);   // шина наша одна, транзакцию держим открытой
                                     // и начинаем на медленной тактовой

  const uint8_t *p = INIT_SEQ;
  while (true) {
    const uint8_t cmd = pgm_read_byte(p++);
    if (cmd == 0x00) break;                  // 0x00 в таблице не встречается
    const uint8_t len = pgm_read_byte(p++);

    writeCmd(cmd);
    uint8_t buf[16];
    uint8_t n = len & 0x7F;
    if (n > sizeof(buf)) n = sizeof(buf);    // страховка от битой таблицы
    if (n) {
      for (uint8_t i = 0; i < n; i++) buf[i] = pgm_read_byte(p++);
      writeData(buf, n);
    }
    if (len & 0x80) delay(120);
  }

  // Таблица ушла на медленной тактовой — теперь можно разгонять шину.
  // Дальше по ней идут только пиксели, а им сбой не смертелен: в худшем
  // случае будет рябь, а не мёртвый экран.
  setClock(SPI_HZ);

  if (PIN_BL >= 0) {
    pinMode(PIN_BL, OUTPUT);
    digitalWrite(PIN_BL, HIGH);
  }
}

void setWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  uint8_t d[4];

  writeCmd(0x2A);                          // column address set
  d[0] = x0 >> 8; d[1] = x0 & 0xFF;
  d[2] = x1 >> 8; d[3] = x1 & 0xFF;
  writeData(d, 4);

  writeCmd(0x2B);                          // row address set
  d[0] = y0 >> 8; d[1] = y0 & 0xFF;
  d[2] = y1 >> 8; d[3] = y1 & 0xFF;
  writeData(d, 4);

  writeCmd(0x2C);                          // memory write
}

void pushPixels(const uint16_t *data, size_t count) {
  dcData();
  csLow();
  bus->writeBytes(reinterpret_cast<const uint8_t *>(data), count * 2);
  csHigh();
}

void fill(uint16_t color) {
  static uint16_t row[TFT_W];
  const uint16_t be = swap565(color);
  for (int i = 0; i < TFT_W; i++) row[i] = be;

  setWindow(0, 0, TFT_W - 1, TFT_H - 1);
  for (int y = 0; y < TFT_H; y++) pushPixels(row, TFT_W);
}

void setClock(uint32_t hz) {
  if (!bus) return;
  bus->endTransaction();
  settings = SPISettings(hz, MSBFIRST, SPI_MODE0);
  bus->beginTransaction(settings);
}

void backlight(uint8_t duty) {
  if (PIN_BL < 0) return;
  analogWrite(PIN_BL, duty);
}

} // namespace gc9a01
