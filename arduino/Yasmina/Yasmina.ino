// Ясмина — круглый дисплей GC9A01 240x240 плюс голосовой диалог.
//
// Лицо и голос живут на разных ядрах, и это не украшательство: запрос к
// серверу блокирует поток на несколько секунд, воспроизведение — на всё время
// реплики. Если крутить и то и другое в одном loop(), лицо на этих секундах
// замирает — ровно там, где оно нужнее всего. Поэтому:
//
//   ядро 0 — задача лица: только face::tick() и SPI к панели;
//   ядро 1 — loop(): кнопка, запись, HTTP, звук.
//
// Общение между ними — через face::setState/setSpeaking/setThinking: запись
// одного слова, читателю достаточно.
//
// Сценарий (VOICE_ENABLED): обычное лицо → зажал кнопку — слушает →
// отпустил — думает → пришёл ответ — переходит в эмоцию ответа и говорит с ней.
// Без VOICE_ENABLED крутится старое демо по состояниям.

#include <Arduino.h>
#include "config.h"
#include "gc9a01.h"
#include "face.h"
#ifdef VOICE_ENABLED
  #include "voice.h"
#endif

// --- задача лица -----------------------------------------------------------
// Панель инициализируется здесь же, а не в setup(): SPI-транзакция у нас
// открывается один раз и держится открытой, а владеть ей должна та задача,
// которая потом сыплет в шину пиксели.
void faceTask(void *) {
  gc9a01::begin();
  face::begin();
  gc9a01::backlight(255);
  gc9a01::fill(face::backgroundColor());     // фон рисуем один раз, дальше — только лицо
  face::setState(FACE_IDLE, /*instant=*/true);

  for (;;) {
    face::tick();                            // сам себя ограничивает по FPS
    vTaskDelay(1);                           // отдать такт сторожевому таймеру
  }
}

#ifndef VOICE_ENABLED
// --- демо-сценарий ---------------------------------------------------------
// Эмоция и речь — две независимые вещи, поэтому в сценарии есть и молчащие
// состояния, и говорение поверх разных выражений лица.
void demo() {
  struct Step { FaceState state; bool talk; uint32_t ms; };
  static const Step SCRIPT[] = {
    { FACE_IDLE,     false, 3000 },   // ждём
    { FACE_LISTEN,   false, 2500 },   // слушаем
    { FACE_SPEAK,    true,  3000 },   // отвечаем с нейтральным лицом
    { FACE_HAPPY,    false, 2000 },   // радуемся молча
    { FACE_HAPPY,    true,  3000 },   // радуемся и говорим одновременно
    { FACE_LOVE,     false, 2800 },   // гашыйк
    { FACE_WINK,     false, 2200 },   // шаян
    { FACE_SURPRISE, false, 2200 },   // гаҗәп
    { FACE_SAD,      false, 2800 },   // моң
    { FACE_SLEEP,    false, 3500 },   // йокы
  };
  static const size_t N = sizeof(SCRIPT) / sizeof(SCRIPT[0]);

  static size_t   step = 0;
  static uint32_t until = 0;

  if (millis() >= until) {
    face::setState(SCRIPT[step].state);
    face::setSpeaking(SCRIPT[step].talk);
    until = millis() + SCRIPT[step].ms;
    step = (step + 1) % N;
  }
}
#endif

// --- ручное управление из монитора порта ----------------------------------
// Цифры 1..9 — эмоция, t — включить и выключить говорение поверх неё.
void serialCommands() {
  while (Serial.available()) {
    const int c = Serial.read();
    if (c >= '1' && c <= '0' + FACE_STATE_COUNT) {
      face::setState((FaceState)(c - '1'));
    } else if (c == 't') {
      face::setSpeaking(!face::isSpeaking());
    }
  }
}

// Что с памятью на старте. PSRAM самому лицу не нужна — буферы кадра лежат
// во внутренней RAM, она заметно быстрее, — но голосовой части она необходима:
// двадцать секунд записи это 640 КБ, плюс тело запроса и ответ.
void reportMemory() {
  Serial.printf("RAM:   всего %u, свободно %u байт\n",
                (unsigned)ESP.getHeapSize(), (unsigned)ESP.getFreeHeap());
  const size_t psram = ESP.getPsramSize();
  if (psram == 0) {
    Serial.println("PSRAM: не найдена — проверь memory_type = qio_opi "
                   "и флаг -DBOARD_HAS_PSRAM в platformio.ini");
  } else {
    Serial.printf("PSRAM: всего %u, свободно %u байт\n",
                  (unsigned)psram, (unsigned)ESP.getFreePsram());
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  reportMemory();

  // Лицо — на ядро 0, подальше от Wi-Fi-стека и звука, которые сидят в loop().
  xTaskCreatePinnedToCore(faceTask, "face", 8192, nullptr, 2, nullptr, 0);
  delay(300);                                   // дать панели подняться

#ifdef VOICE_ENABLED
  voice::begin();
#endif

  Serial.print("Ясмина готова. Эмоции —");
  for (int i = 0; i < FACE_STATE_COUNT; i++)
    Serial.printf(" %d:%s", i + 1, face::stateName((FaceState)i));
  Serial.println(", t — говорение.");
}

// Раз в 3 секунды печатает реальный FPS — полезно при подборе SPI_HZ.
void reportFps() {
  static uint32_t next = 0;
  if (millis() < next) return;
  next = millis() + 3000;
  const face::Profile p = face::profile();
  Serial.printf("[face] %s%s%s  %.1f fps | контуры %lu мкс, ореол %lu, композит %lu, SPI %lu\n",
                face::stateName(face::state()),
                face::isSpeaking() ? " + речь" : "",
                face::isThinking() ? " + думает" : "", face::fps(),
                (unsigned long)p.rasterUs, (unsigned long)p.glowUs,
                (unsigned long)p.compUs, (unsigned long)p.pushUs);
}

void loop() {
  serialCommands();
#ifdef VOICE_ENABLED
  voice::tick();                 // кнопка, запись, запрос, звук — всё блокирующее здесь
#else
  demo();
  delay(5);
#endif
  reportFps();
}
