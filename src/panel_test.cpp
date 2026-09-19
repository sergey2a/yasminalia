// Тест панели GC9A01 — отдельная прошивка, лицо не рисуется.
// Задача: понять, доходят ли вообще данные до контроллера дисплея.
//
//   pio run -e paneltest -t upload && pio device monitor -e paneltest
//
// Что происходит по шагам (каждый комментируется в монитор порта):
//   0. три раза мигает подсветка — проверка пина BL;
//   1. заливки: красный, зелёный, синий, белый, чёрный;
//   2. маркеры по краям: верх красный, низ синий, лево зелёное, право белое;
//   3. шахматка — по ней видно рябь и битые строки на быстрой шине.

#include <Arduino.h>
#include "config.h"
#include "gc9a01.h"

static void fillRect(int x, int y, int w, int h, uint16_t color) {
  static uint16_t row[TFT_W];
  const uint16_t be = gc9a01::swap565(color);
  for (int i = 0; i < w && i < TFT_W; i++) row[i] = be;
  gc9a01::setWindow(x, y, x + w - 1, y + h - 1);
  for (int i = 0; i < h; i++) gc9a01::pushPixels(row, w);
}

static void solid(const char *name, uint16_t color) {
  Serial.printf("  заливка: %s\n", name);
  gc9a01::fill(color);
  delay(1200);
}

void setup() {
  Serial.begin(115200);
  delay(400);
  Serial.println("\n=== Тест панели GC9A01 ===");
  Serial.printf("SCK=%d MOSI=%d CS=%d DC=%d RST=%d BL=%d, шина %d Гц\n",
                PIN_SCK, PIN_MOSI, PIN_CS, PIN_DC, PIN_RST, PIN_BL, SPI_HZ);

  Serial.println("шаг 0: мигаю подсветкой три раза");
  pinMode(PIN_BL, OUTPUT);
  for (int i = 0; i < 3; i++) {
    digitalWrite(PIN_BL, LOW);  delay(350);
    digitalWrite(PIN_BL, HIGH); delay(350);
  }
  Serial.println("       если яркость не менялась — BL не на этом пине "
                 "или подсветка запитана напрямую");

  Serial.println("шаг 1: инициализация дисплея");
  gc9a01::begin();
  Serial.println("       инициализация отправлена");
}

void loop() {
  Serial.println("шаг 2: сплошные заливки");
  solid("красный", gc9a01::rgb565(255, 0, 0));
  solid("зелёный", gc9a01::rgb565(0, 255, 0));
  solid("синий",   gc9a01::rgb565(0, 0, 255));
  solid("белый",   gc9a01::rgb565(255, 255, 255));
  solid("чёрный",  0x0000);

  Serial.println("шаг 3: маркеры краёв — верх красный, низ синий, "
                 "слева зелёный, справа белый");
  gc9a01::fill(0x0000);
  fillRect(90,   0, 60, 24, gc9a01::rgb565(255, 0, 0));     // верх
  fillRect(90, 216, 60, 24, gc9a01::rgb565(0, 0, 255));     // низ
  fillRect(0,   90, 24, 60, gc9a01::rgb565(0, 255, 0));     // слева
  fillRect(216, 90, 24, 60, gc9a01::rgb565(255, 255, 255)); // справа
  delay(4000);

  Serial.println("шаг 4: шахматка 8x8 — ищем рябь и рваные строки");
  static uint16_t line[TFT_W];
  gc9a01::setWindow(0, 0, TFT_W - 1, TFT_H - 1);
  for (int y = 0; y < TFT_H; y++) {
    for (int x = 0; x < TFT_W; x++) {
      const bool on = ((x >> 3) + (y >> 3)) & 1;
      line[x] = gc9a01::swap565(on ? gc9a01::rgb565(255, 200, 60) : 0x0000);
    }
    gc9a01::pushPixels(line, TFT_W);
  }
  delay(4000);
}
