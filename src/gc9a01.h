#pragma once
#include <Arduino.h>

// Минимальный драйвер GC9A01: инициализация, окно вывода, поток пикселей.
// Ничего лишнего — вся графика рисуется нами в face.cpp.
namespace gc9a01 {

void begin();

// Окно, в которое пойдут следующие пиксели (координаты включительно).
void setWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

// Пиксели RGB565, УЖЕ переставленные в big-endian (см. swap565()).
void pushPixels(const uint16_t *data, size_t count);

// Заливка всего экрана одним цветом (обычный little-endian RGB565).
void fill(uint16_t color);

// Подсветка: 0..255. Без PIN_BL — no-op.
void backlight(uint8_t duty);

// Сменить тактовую шины на лету — нужно, чтобы нащупать потолок пайки.
void setClock(uint32_t hz);

inline uint16_t swap565(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }

inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)((r & 0xF8) << 8 | (g & 0xFC) << 3 | (b >> 3));
}

} // namespace gc9a01
