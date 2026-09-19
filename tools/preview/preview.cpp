// Хост-превью лица Ясмины: компилирует настоящий src/face.cpp, подсовывает ему
// вместо панели обычный буфер в памяти и сохраняет кадры в PPM.
// Нужно, чтобы подбирать форму глаз, не перепрошивая колонку.
//
// Сборка и запуск: tools/preview/build.sh

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "config.h"
#include "gc9a01.h"
#include "face.h"

uint32_t g_virtualMs = 0;

// --- панель, которой нет: пиксели просто ложатся в массив -------------------
static uint16_t fb[TFT_W * TFT_H];
static uint16_t winX0, winY0, winX1, winY1, winX, winY;

namespace gc9a01 {
void begin() {}
void backlight(uint8_t) {}

void setWindow(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
  winX0 = x0; winY0 = y0; winX1 = x1; winY1 = y1;
  winX = x0; winY = y0;
}

void pushPixels(const uint16_t *data, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (winY <= winY1 && winX < TFT_W && winY < TFT_H)
      fb[winY * TFT_W + winX] = swap565(data[i]);   // возвращаем байты обратно
    if (++winX > winX1) { winX = winX0; winY++; }
  }
}

void fill(uint16_t color) {
  for (int i = 0; i < TFT_W * TFT_H; i++) fb[i] = color;
}
} // namespace gc9a01

// --- сохранение кадра -------------------------------------------------------
static void savePPM(const std::string &path) {
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) { perror("fopen"); return; }
  fprintf(f, "P6\n%d %d\n255\n", TFT_W, TFT_H);
  for (int y = 0; y < TFT_H; y++) {
    for (int x = 0; x < TFT_W; x++) {
      // круглая панель: всё, что вне окружности, физически не видно
      const float dx = x - 119.5f, dy = y - 119.5f;
      uint16_t c = (dx * dx + dy * dy > 120.f * 120.f) ? 0 : fb[y * TFT_W + x];
      const uint8_t rgb[3] = {
        (uint8_t)((c >> 11 & 0x1F) * 255 / 31),
        (uint8_t)((c >>  5 & 0x3F) * 255 / 63),
        (uint8_t)((c       & 0x1F) * 255 / 31),
      };
      fwrite(rgb, 1, 3, f);
    }
  }
  fclose(f);
}

// Прокрутить анимацию на ms виртуального времени с шагом 20 мс (50 к/с).
static void advance(uint32_t ms) {
  for (uint32_t t = 0; t < ms; t += 20) {
    g_virtualMs += 20;
    face::tick();
  }
}

int main(int argc, char **argv) {
  const std::string out = argc > 1 ? argv[1] : ".";

  face::begin();
  gc9a01::fill(face::backgroundColor());

  // 1. Портреты всех состояний — молча и с речью: говорить можно с любым лицом.
  for (int si = 0; si < FACE_STATE_COUNT; si++) {
    const FaceState s = (FaceState)si;
    face::setSpeaking(false);
    face::setSpeechLevel(-1.f);
    face::setState(s, /*instant=*/true);
    advance(600);                       // дать дыханию и рту выйти на режим
    savePPM(out + "/face_" + face::stateName(s) + ".ppm");

    face::setSpeaking(true);
    face::setSpeechLevel(0.85f);        // фиксируем раскрытие, чтобы кадр был сравним
    advance(600);
    savePPM(out + "/talk_" + face::stateName(s) + ".ppm");
  }
  face::setSpeaking(false);
  face::setSpeechLevel(-1.f);

  // 2. Последовательность кадров демо-сценария — на анимацию.
  face::setState(FACE_IDLE, true);
  int n = 0;
  struct Step { FaceState s; uint32_t ms; };
  const Step script[] = {
    { FACE_IDLE, 1600 }, { FACE_LISTEN, 1600 }, { FACE_SPEAK, 1800 }, { FACE_HAPPY, 1600 },
    { FACE_LOVE, 1800 }, { FACE_WINK, 1600 }, { FACE_SURPRISE, 1600 },
    { FACE_SLEEP, 1800 }, { FACE_SAD, 1800 },
  };
  for (const Step &st : script) {
    face::setState(st.s);
    for (uint32_t t = 0; t < st.ms; t += 40) {
      g_virtualMs += 40;
      face::tick();
      char name[256];
      snprintf(name, sizeof(name), "%s/anim_%04d.ppm", out.c_str(), n++);
      savePPM(name);
    }
  }
  printf("кадров сохранено: %d\n", n);
  return 0;
}
