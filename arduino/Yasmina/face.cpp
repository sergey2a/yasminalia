// Лицо считается на каждом кадре целиком, и разница между -Os и -O2 здесь
// это примерно полтора десятка кадров в секунду. PlatformIO ставит -O2 из
// platformio.ini, а в Arduino IDE флаги компилятора мышкой не поменяешь —
// поэтому просим оптимизацию прямо отсюда, для обеих сборок одинаково.
#pragma GCC optimize("O2")

#include "face.h"
#include "shapes.h"
#include "config.h"
#include "gc9a01.h"
#include <math.h>
#include <string.h>
#include <esp_system.h>

// ---------------------------------------------------------------------------
// Лицо собирается из замкнутых контуров Безье (см. shapes.cpp) и рисуется в три
// прохода — так же, как это делают растеризаторы шрифтов:
//
//   1. контуры разбиваются на отрезки и заливаются по строкам в маску покрытия
//      (8 бит на пиксель, 4 подстроки на строку — отсюда сглаженный край);
//   2. маска уменьшается вдвое и дважды размывается box-фильтром — это ореол
//      (считать честное поле расстояний до кривой на каждый пиксель дороже
//      всего кадра, а глазу разницы нет);
//   3. маска и ореол смешиваются с фоном и уходят в панель полосами.
//
// Цвета сняты с эталонного рендера колонки: заливка RGB(254,225,164),
// ореол RGB(255,126,15) с затуханием примерно exp(-d/7).
// ---------------------------------------------------------------------------

namespace face {
namespace {

// ---------------------------- палитра --------------------------------------
// Цвета черт, украшения и ореола лежат в самом состоянии (shapes.cpp) и
// интерполируются при переходе, поэтому смена настроения — это плавная
// заливка, а не рывок. Здесь остаётся только фон.
constexpr Rgb C_BG = { 10.f, 8.f, 6.f };

constexpr float GLOW_GAIN = 1.05f;      // подобрано по яркости ореола эталона

// ---------------------------- буферы ---------------------------------------
constexpr int FACE_H = FACE_Y1 - FACE_Y0;      // высота живой полосы экрана
constexpr int GW = TFT_W / 2, GH = FACE_H / 2; // ореол считается вполовинном разрешении
constexpr int SUBS = 4;                        // подстрок на строку при заливке
constexpr float SUB_W = 255.f / SUBS;          // вклад одной подстроки

uint8_t  mask[TFT_W * FACE_H];
// Какие пиксели маски принадлежат украшению, а не чертам лица: по биту на
// пиксель, всего 4 КБ. Отдельная полноценная маска стоила бы 34 КБ, а разница
// нужна только чтобы выбрать цвет — покрытие и ореол у слоёв общие.
constexpr int DECOR_STRIDE = TFT_W / 8;
uint8_t  decorBits[DECOR_STRIDE * FACE_H];
uint8_t  glowA[GW * GH], glowB[GW * GH];
uint16_t band[TFT_W * BAND_H];

// Таблицы для билинейной выборки ореола: считаются один раз.
int16_t gxIdx[TFT_W];  uint8_t gxFrac[TFT_W];
int16_t gyIdx[FACE_H]; uint8_t gyFrac[FACE_H];

// ---------------------------- рёбра ----------------------------------------
constexpr int FLATTEN  = 8;                       // отрезков на кривую
constexpr int MAX_EDGES = 5 * PATH_SEGS * FLATTEN + 8;

struct Edge {
  float y0, y1;      // всегда y0 < y1
  float x0, dxdy;    // x на высоте y = x0 + (y - y0) * dxdy
  int8_t dir;        // +1 если ребро шло вниз, -1 если вверх
};

Edge edges[MAX_EDGES];
int  nEdges = 0;

// Рёбра разложены по строке, с которой начинаются: на каждой подстроке
// перебирается только несколько активных рёбер, а не все двести с лишним.
int16_t bucketHead[FACE_H];
int16_t edgeNext[MAX_EDGES];
uint16_t active[MAX_EDGES];

// ---------------------------- состояние рантайма ---------------------------
Look      cur;
Look      tgt;
FaceState curState = FACE_IDLE;

float    gazeX = 0, gazeY = 0, gazeTX = 0, gazeTY = 0;
uint32_t gazeNextMs = 0;
bool     thinking = false;      // ждём ответ: взгляд уходит вверх и плывёт

float    blink = 0;
uint32_t blinkNextMs = 0;
uint8_t  blinkPhase = 0;

float speechLevel = -1.f;      // <0 — внешний уровень не задан, болтаем сами
float speakPhase  = 0.f;
float mouthOpen   = 0.f;       // текущее раскрытие рта, 0..1
bool  speaking    = false;     // говорим ли прямо сейчас, независимо от эмоции

uint32_t lastFrameMs = 0, fpsMark = 0, fpsCount = 0;
float    fpsValue = 0.f, clockT = 0.f;

// Профиль кадра: сколько микросекунд уходит на каждую стадию.
uint32_t accRaster = 0, accGlow = 0, accComp = 0, accPush = 0;
Profile  prof = {0, 0, 0, 0};

// ---------------------------- мелочи ---------------------------------------
inline float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
inline int   clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---------------------------- сборка контуров ------------------------------
inline void pushEdge(float ax, float ay, float bx, float by) {
  if (ay == by || nEdges >= MAX_EDGES) return;    // горизонтальные не нужны
  Edge &e = edges[nEdges++];
  if (ay < by) { e.y0 = ay; e.y1 = by; e.x0 = ax; e.dir = +1; e.dxdy = (bx - ax) / (by - ay); }
  else         { e.y0 = by; e.y1 = ay; e.x0 = bx; e.dir = -1; e.dxdy = (ax - bx) / (ay - by); }
}

// Контур в рёбра: кубики разбиваются на FLATTEN отрезков.
// sx = -1 зеркалит черту на левую половину лица, sy сплющивает при моргании.
void emitPath(const Path &path, float ox, float oy, float sx, float sy) {
  float px = ox + sx * path.p[PATH_PTS - 1].x;
  float py = oy + sy * path.p[PATH_PTS - 1].y;

  for (int s = 0; s < PATH_SEGS; s++) {
    const Pt &c1 = path.p[s * 3 + 0];
    const Pt &c2 = path.p[s * 3 + 1];
    const Pt &to = path.p[s * 3 + 2];

    const float x1 = ox + sx * c1.x, y1 = oy + sy * c1.y;
    const float x2 = ox + sx * c2.x, y2 = oy + sy * c2.y;
    const float x3 = ox + sx * to.x, y3 = oy + sy * to.y;
    const float x0 = px, y0 = py;

    for (int i = 1; i <= FLATTEN; i++) {
      const float t = (float)i / FLATTEN, u = 1.f - t;
      const float a = u * u * u, b = 3.f * u * u * t, c = 3.f * u * t * t, d = t * t * t;
      const float nx = a * x0 + b * x1 + c * x2 + d * x3;
      const float ny = a * y0 + b * y1 + c * y2 + d * y3;
      pushEdge(px, py, nx, ny);
      px = nx; py = ny;
    }
  }
}

// Рот: смесь сомкнутого и раскрытого контуров ТЕКУЩЕЙ эмоции по громкости.
// Оба контура свои у каждого состояния, поэтому говорить можно с любым
// выражением лица и форма не разъезжается на промежуточных кадрах.
void emitMouth(float ox, float oy) {
  if (mouthOpen < 0.005f) { emitPath(cur.mouth, ox, oy, 1.f, 1.f); return; }
  Path m;
  for (int i = 0; i < PATH_PTS; i++) {
    m.p[i].x = lerpf(cur.mouth.p[i].x, cur.mouthTalk.p[i].x, mouthOpen);
    m.p[i].y = lerpf(cur.mouth.p[i].y, cur.mouthTalk.p[i].y, mouthOpen);
  }
  emitPath(m, ox, oy, 1.f, 1.f);
}

// Черты лица. Подмигивание сплющивает только правый глаз — левый остаётся
// открытым, поэтому асимметрия достигается одним числом, без второго контура.
void buildFaceEdges() {
  nEdges = 0;
  const float breath = sinf(clockT * 1.6f);
  const float eyeSY  = 1.f - blink * 0.90f;
  const float winkSY = eyeSY * (1.f - clamp01(cur.wink) * 0.88f);
  const float cx     = TFT_W * 0.5f;

  const float eyeY  = cur.eyeAt.y + gazeY + breath * 1.0f;
  const float browY = cur.browAt.y + gazeY * 0.6f + breath * 0.8f;

  emitPath(cur.eye,  cx + gazeX + cur.eyeAt.x,  eyeY,  +1.f, winkSY);
  emitPath(cur.eye,  cx + gazeX - cur.eyeAt.x,  eyeY,  -1.f, eyeSY);
  emitPath(cur.brow, cx + gazeX * 0.8f + cur.browAt.x, browY, +1.f, 1.f);
  emitPath(cur.brow, cx + gazeX * 0.8f - cur.browAt.x, browY, -1.f, 1.f);
  emitMouth(cx + gazeX * 0.35f + cur.mouthAt.x, cur.mouthAt.y);
}

// Украшение: сердечко, месяц, слезинка, искра. Рисуется парой, зеркально;
// если якорь стоит по центру (x = 0), обе копии совпадут в одну фигуру.
void buildDecorEdges() {
  nEdges = 0;
  if (clamp01(cur.decorOn) < 0.02f) return;
  const float breath = sinf(clockT * 1.6f);
  const float cx = TFT_W * 0.5f;
  const float y  = cur.decorAt.y + gazeY * 0.4f + breath * 1.4f;
  emitPath(cur.decor, cx + gazeX * 0.5f + cur.decorAt.x, y, +1.f, 1.f);
  // Несимметричную фигуру по центру (месяц) зеркалить нельзя — из неё
  // получатся две половинки, развёрнутые друг к другу.
  if (cur.decorMirror > 0.5f)
    emitPath(cur.decor, cx + gazeX * 0.5f - cur.decorAt.x, y, -1.f, 1.f);
}

// ---------------------------- заливка --------------------------------------
inline void addCov(uint8_t &dst, float v) {
  const int n = dst + (int)(v + 0.5f);
  dst = (uint8_t)(n > 255 ? 255 : n);
}

uint8_t *decorRow = nullptr;      // не null — заливаем слой украшения

void addSpan(uint8_t *row, float xa, float xb) {
  if (xb <= 0.f || xa >= (float)TFT_W || xb <= xa) return;
  if (xa < 0.f) xa = 0.f;
  if (xb > (float)TFT_W) xb = (float)TFT_W;

  int ia = (int)xa, ib = (int)xb;
  if (ib >= TFT_W) ib = TFT_W - 1;
  if (decorRow)
    for (int i = ia; i <= ib; i++) decorRow[i >> 3] |= (uint8_t)(1 << (i & 7));

  if (ia == ib) { addCov(row[ia], SUB_W * (xb - xa)); return; }

  addCov(row[ia], SUB_W * (ia + 1 - xa));            // частичное покрытие слева
  for (int i = ia + 1; i < ib; i++) addCov(row[i], SUB_W);
  addCov(row[ib], SUB_W * (xb - ib));                // и справа
}

void bucketEdges() {
  for (int i = 0; i < FACE_H; i++) bucketHead[i] = -1;
  for (int e = 0; e < nEdges; e++) {
    int row = (int)edges[e].y0 - FACE_Y0;
    if (row < 0) row = 0;
    if (row >= FACE_H) continue;                     // ребро ниже живой полосы
    edgeNext[e] = bucketHead[row];
    bucketHead[row] = (int16_t)e;
  }
}

void rasterize(bool decorLayer) {
  bucketEdges();

  float  xs[32];
  int8_t dirs[32];
  int    nActive = 0;

  for (int row = 0; row < FACE_H; row++) {
    uint8_t *mrow = mask + row * TFT_W;
    decorRow = decorLayer ? (decorBits + row * DECOR_STRIDE) : nullptr;

    for (int e = bucketHead[row]; e >= 0; e = edgeNext[e])
      if (nActive < MAX_EDGES) active[nActive++] = (uint16_t)e;

    for (int s = 0; s < SUBS; s++) {
      const float y = (float)(FACE_Y0 + row) + (s + 0.5f) / SUBS;

      int n = 0;
      for (int k = 0; k < nActive && n < 32; k++) {
        const Edge &E = edges[active[k]];
        if (y < E.y0 || y >= E.y1) continue;
        const float x = E.x0 + (y - E.y0) * E.dxdy;
        int j = n++;                                  // вставкой, пересечений мало
        while (j > 0 && xs[j - 1] > x) { xs[j] = xs[j - 1]; dirs[j] = dirs[j - 1]; j--; }
        xs[j] = x; dirs[j] = E.dir;
      }

      // Правило ненулевого индекса: пересекающиеся контуры объединяются,
      // а не вычитаются друг из друга.
      int wind = 0;
      float spanStart = 0.f;
      for (int i = 0; i < n; i++) {
        const int prev = wind;
        wind += dirs[i];
        if (prev == 0 && wind != 0)      spanStart = xs[i];
        else if (prev != 0 && wind == 0) addSpan(mrow, spanStart, xs[i]);
      }
    }

    // выбросить рёбра, закончившиеся на этой строке
    const float rowBottom = (float)(FACE_Y0 + row + 1);
    int keep = 0;
    for (int k = 0; k < nActive; k++)
      if (edges[active[k]].y1 > rowBottom) active[keep++] = active[k];
    nActive = keep;
  }
  decorRow = nullptr;
}

// ---------------------------- ореол ----------------------------------------
void blurLine(const uint8_t *src, uint8_t *dst, int n, int stride, int r) {
  const int win   = 2 * r + 1;
  const int recip = (1 << 16) / win;            // деление на окно — умножением
  const int first = src[0], last = src[(n - 1) * stride];

  int sum = first * (r + 1);
  for (int i = 1; i <= r; i++) sum += src[i * stride];

  for (int i = 0; i < n; i++) {
    dst[i * stride] = (uint8_t)((sum * recip) >> 16);
    sum += (i + r + 1 < n) ? src[(i + r + 1) * stride] : last;
    sum -= (i - r >= 0)    ? src[(i - r) * stride]     : first;
  }
}

void buildGlow() {
  // уменьшение вдвое: четыре пикселя маски в один пиксель ореола
  for (int y = 0; y < GH; y++) {
    const uint8_t *r0 = mask + (y * 2) * TFT_W;
    const uint8_t *r1 = r0 + TFT_W;
    uint8_t *dst = glowA + y * GW;
    for (int x = 0; x < GW; x++) {
      const int i = x * 2;
      dst[x] = (uint8_t)((r0[i] + r0[i + 1] + r1[i] + r1[i + 1]) >> 2);
    }
  }
  // два прохода box-фильтра по обеим осям ≈ гауссиана нужной ширины
  constexpr int R = 3;
  for (int pass = 0; pass < 2; pass++) {
    for (int y = 0; y < GH; y++) blurLine(glowA + y * GW, glowB + y * GW, GW, 1, R);
    for (int x = 0; x < GW; x++) blurLine(glowB + x, glowA + x, GH, GW, R);
  }
}

void buildSampleTables() {
  for (int x = 0; x < TFT_W; x++) {
    const float g = x * 0.5f - 0.5f;
    const int i = (int)floorf(g);
    gxIdx[x] = (int16_t)clampi(i, 0, GW - 2);
    gxFrac[x] = (uint8_t)clampi((int)((g - gxIdx[x]) * 256.f), 0, 255);
  }
  for (int y = 0; y < FACE_H; y++) {
    const float g = y * 0.5f - 0.5f;
    const int i = (int)floorf(g);
    gyIdx[y] = (int16_t)clampi(i, 0, GH - 2);
    gyFrac[y] = (uint8_t)clampi((int)((g - gyIdx[y]) * 256.f), 0, 255);
  }
}

// ---------------------------- вывод ----------------------------------------
void compositeBand(int y0, int rows) {
  uint16_t *out = band;

  // Целочисленные константы блендинга: всё считается в 0..255 без float.
  const int glowScale = (int)(GLOW_GAIN * cur.glow * 256.f);
  const int bgR = (int)C_BG.r, bgG = (int)C_BG.g, bgB = (int)C_BG.b;
  const int glR = (int)cur.glowInk.r, glG = (int)cur.glowInk.g, glB = (int)cur.glowInk.b;
  const int inR = (int)cur.ink.r, inG = (int)cur.ink.g, inB = (int)cur.ink.b;
  const int dcR = (int)cur.decorInk.r, dcG = (int)cur.decorInk.g, dcB = (int)cur.decorInk.b;
  const uint16_t bgPixel = gc9a01::swap565(gc9a01::rgb565(bgR, bgG, bgB));

  for (int r = 0; r < rows; r++) {
    const int faceRow = y0 + r - FACE_Y0;
    const uint8_t *mrow = mask + faceRow * TFT_W;
    const uint8_t *drow = decorBits + faceRow * DECOR_STRIDE;
    const uint8_t *g0 = glowA + gyIdx[faceRow] * GW;
    const uint8_t *g1 = g0 + GW;
    const int fy = gyFrac[faceRow];

    // Полоса, где вообще есть свет: за её пределами строка — ровный фон.
    int lo = GW, hi = -1;
    for (int i = 0; i < GW; i++)
      if (g0[i] | g1[i]) { if (i < lo) lo = i; hi = i; }

    if (hi < 0) {
      for (int x = 0; x < TFT_W; x++) out[x] = bgPixel;
      out += TFT_W;
      continue;
    }
    const int xa = clampi((lo - 1) * 2, 0, TFT_W);
    const int xb = clampi((hi + 2) * 2, 0, TFT_W);

    for (int x = 0; x < xa; x++) out[x] = bgPixel;

    for (int x = xa; x < xb; x++) {
      const int gx = gxIdx[x], fx = gxFrac[x];
      const int t = g0[gx] + (((g0[gx + 1] - g0[gx]) * fx) >> 8);
      const int b = g1[gx] + (((g1[gx + 1] - g1[gx]) * fx) >> 8);
      const int lit = ((t + (((b - t) * fy) >> 8)) * glowScale) >> 8;

      int R = bgR + ((glR * lit) >> 8);
      int G = bgG + ((glG * lit) >> 8);
      int B = bgB + ((glB * lit) >> 8);

      const int a = mrow[x];
      if (a) {
        const bool dec = drow[x >> 3] & (1 << (x & 7));
        const int kR = dec ? dcR : inR, kG = dec ? dcG : inG, kB = dec ? dcB : inB;
        R += ((kR - R) * a) >> 8;
        G += ((kG - G) * a) >> 8;
        B += ((kB - B) * a) >> 8;
      }
      if (R > 255) R = 255;
      if (G > 255) G = 255;
      if (B > 255) B = 255;
      out[x] = gc9a01::swap565((uint16_t)((R & 0xF8) << 8 | (G & 0xFC) << 3 | (B >> 3)));
    }

    for (int x = xb; x < TFT_W; x++) out[x] = bgPixel;
    out += TFT_W;
  }
}

// ---------------------------- анимация -------------------------------------
void morph(float dt) {
  const float k = 1.f - expf(-dt * 9.f);          // гладко при любом FPS
  float *c = reinterpret_cast<float *>(&cur);
  const float *t = reinterpret_cast<const float *>(&tgt);
  for (size_t i = 0; i < sizeof(Look) / sizeof(float); i++)
    c[i] = lerpf(c[i], t[i], k);
}

void animateGaze(float dt, uint32_t now) {
  if (thinking) {
    // Взгляд ходит по вытянутой дуге в верхней части — два несоизмеримых
    // синуса, чтобы траектория не читалась как петля.
    gazeTX = sinf(clockT * 0.9f) * 0.85f + sinf(clockT * 0.37f) * 0.15f;
    gazeTY = -0.75f + sinf(clockT * 0.6f) * 0.25f;
    gazeNextMs = now;                             // после размышления — сразу новая цель
    const float kt = 1.f - expf(-dt * 2.2f);      // медленнее обычного
    gazeX = lerpf(gazeX, gazeTX * tgt.gazeAmp, kt);
    gazeY = lerpf(gazeY, gazeTY * tgt.gazeAmp * 0.9f, kt);
    return;
  }
  if (now >= gazeNextMs) {
    gazeTX = random(-100, 101) / 100.f;
    gazeTY = random(-60, 61) / 100.f;
    gazeNextMs = now + random(1200, 3600);
  }
  const float k = 1.f - expf(-dt * 4.f);
  gazeX = lerpf(gazeX, gazeTX * tgt.gazeAmp, k);
  gazeY = lerpf(gazeY, gazeTY * tgt.gazeAmp * 0.5f, k);
}

void animateBlink(float dt, uint32_t now) {
  if (tgt.blinkEvery <= 0.f) { blink = 0.f; return; }
  switch (blinkPhase) {
    case 0: if (now >= blinkNextMs) blinkPhase = 1; break;
    case 1:
      blink += dt * 16.f;
      if (blink >= 1.f) { blink = 1.f; blinkPhase = 2; }
      break;
    default:
      blink -= dt * 9.f;
      if (blink <= 0.f) {
        blink = 0.f; blinkPhase = 0;
        const uint32_t base = (uint32_t)(tgt.blinkEvery * 1000.f);
        blinkNextMs = now + random(base / 2, base * 2);
      }
      break;
  }
}

void animateMouth(float dt) {
  // Говорение не привязано к эмоции: оно включается либо явно через
  // setSpeaking(), либо самим состоянием FACE_SPEAK — это просто «нейтральное
  // говорящее лицо», такая же эмоция, как остальные.
  const bool talking = speaking || curState == FACE_SPEAK;

  float target = 0.f;
  if (talking) {
    if (speechLevel >= 0.f) {
      target = clamp01(speechLevel);              // ведём рот по громкости TTS
    } else {
      speakPhase += dt;                           // своя «болтовня»
      const float s = sinf(speakPhase * 11.f) * 0.5f + sinf(speakPhase * 6.3f) * 0.3f;
      target = clamp01(0.45f + s * 0.55f);
    }
  }
  mouthOpen = lerpf(mouthOpen, target, 1.f - expf(-dt * 16.f));
}

} // namespace

// ---------------------------- публичный интерфейс --------------------------
uint16_t backgroundColor() {
  return gc9a01::rgb565((uint8_t)C_BG.r, (uint8_t)C_BG.g, (uint8_t)C_BG.b);
}

void begin() {
  randomSeed(esp_random());
  buildSampleTables();
  cur = tgt = LOOK[FACE_IDLE];
  curState = FACE_IDLE;
  lastFrameMs = millis();
  blinkNextMs = lastFrameMs + 1500;
}

void setState(FaceState s, bool instant) {
  if (s >= FACE_STATE_COUNT) return;
  curState = s;
  tgt = LOOK[s];
  if (instant) cur = tgt;
}

FaceState state() { return curState; }
void setSpeaking(bool on) {
  if (on && !speaking) speakPhase = 0.f;
  speaking = on;
}

bool isSpeaking() { return speaking || curState == FACE_SPEAK; }

void setThinking(bool on) { thinking = on; }
bool isThinking() { return thinking; }

void setSpeechLevel(float level) { speechLevel = level; }
float fps() { return fpsValue; }
Profile profile() { return prof; }

const char *stateName(FaceState s) {
  switch (s) {
    case FACE_IDLE:   return "idle";
    case FACE_LISTEN: return "listen";
    case FACE_SPEAK:  return "speak";
    case FACE_HAPPY:    return "happy";
    case FACE_LOVE:     return "love";
    case FACE_WINK:     return "wink";
    case FACE_SURPRISE: return "surprise";
    case FACE_SLEEP:    return "sleep";
    case FACE_SAD:      return "sad";
    default:            return "?";
  }
}

void tick() {
  const uint32_t now = millis();
  float dt = (now - lastFrameMs) * 0.001f;
  if (dt < 0.012f) return;                     // потолок ~80 к/с
  if (dt > 0.1f) dt = 0.1f;
  lastFrameMs = now;
  clockT += dt;

  morph(dt);
  animateGaze(dt, now);
  animateBlink(dt, now);
  animateMouth(dt);

  uint32_t t0 = micros();
  memset(mask, 0, sizeof(mask));
  memset(decorBits, 0, sizeof(decorBits));
  buildFaceEdges();
  rasterize(false);
  buildDecorEdges();
  rasterize(true);
  const uint32_t t1 = micros();
  buildGlow();
  const uint32_t t2 = micros();
  accRaster += t1 - t0;
  accGlow   += t2 - t1;

  for (int y = FACE_Y0; y < FACE_Y1; y += BAND_H) {
    const int rows = (FACE_Y1 - y) < BAND_H ? (FACE_Y1 - y) : BAND_H;
    const uint32_t c0 = micros();
    compositeBand(y, rows);
    const uint32_t c1 = micros();
    gc9a01::setWindow(0, y, TFT_W - 1, y + rows - 1);
    gc9a01::pushPixels(band, (size_t)TFT_W * rows);
    accComp += c1 - c0;
    accPush += micros() - c1;
  }

  if (++fpsCount >= 30) {
    const uint32_t dtMs = now - fpsMark;
    if (dtMs) fpsValue = fpsCount * 1000.f / dtMs;
    prof.rasterUs = accRaster / fpsCount;
    prof.glowUs   = accGlow / fpsCount;
    prof.compUs   = accComp / fpsCount;
    prof.pushUs   = accPush / fpsCount;
    accRaster = accGlow = accComp = accPush = 0;
    fpsMark = now;
    fpsCount = 0;
  }
}

} // namespace face
