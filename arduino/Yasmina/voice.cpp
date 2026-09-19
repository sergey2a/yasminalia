#include "voice.h"
#include "config.h"
#include "face.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <math.h>
#include "driver/i2s.h"
#include "esp_heap_caps.h"

// ---------------------------------------------------------------------------
// Железо здесь ровно то же, что в плате Сергея (github.com/sergey2a/yasminalia):
// INMP441 на I2S0, MAX98357 на I2S1, сенсорная кнопка TTP223. Пины взяты из
// config.h и совпадают с его распиновкой — провода уже впаяны.
//
// Отличия от его прошивки по существу:
//   * микрофон читается пачками по 256 сэмплов, а не по одному 32-битному
//     слову: вызов i2s_read стоит дороже самого чтения, и на реплике в 20 с
//     штучное чтение теряло сэмплы, отчего речь уезжала по темпу;
//   * частота дискретизации на воспроизведении меняется через i2s_set_clk,
//     а не переустановкой драйвера — переустановка роняет DMA и даёт щелчок;
//   * тело ответа не кладётся в String, а стримится в PSRAM: WAV на несколько
//     секунд — это сотни килобайт, во внутренней куче их просто нет;
//   * JSON разбирается zero-copy прямо по этому буферу, поэтому base64 не
//     копируется ещё раз.
// ---------------------------------------------------------------------------

namespace voice {
namespace {

constexpr uint32_t SR          = AUDIO_SAMPLE_RATE;
constexpr size_t   MAX_SAMPLES = (size_t)SR * AUDIO_MAX_SEC;
constexpr size_t   MIN_SAMPLES = SR / 4;          // короче четверти секунды — промах по кнопке
constexpr uint32_t SLEEP_AFTER_MS = 90000;        // столько тишины — и Ясмина засыпает

int16_t *pcmBuf = nullptr;                        // сырая запись, PSRAM
String   sessionId;                               // сервер держит контекст диалога
const char *stage = "старт";
uint32_t lastActivityMs = 0;

// --------------------------- память ----------------------------------------
void *psAlloc(size_t n) {
  void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : malloc(n);                       // без PSRAM хоть что-то, но недолго
}

// --------------------------- I2S -------------------------------------------
void initMic() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX);
  cfg.sample_rate = SR;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT;   // INMP441 отдаёт 24 бита в 32-битном слоте
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr);

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = PIN_MIC_SCK;
  pins.ws_io_num  = PIN_MIC_WS;
  pins.data_out_num = I2S_PIN_NO_CHANGE;
  pins.data_in_num  = PIN_MIC_SD;
  i2s_set_pin(I2S_NUM_0, &pins);
  i2s_zero_dma_buffer(I2S_NUM_0);
}

uint32_t spkRate = SR;

void initSpeaker() {
  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = SR;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  cfg.dma_buf_count = 6;                            // меньше буфер — меньше отставание рта от звука
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  i2s_driver_install(I2S_NUM_1, &cfg, 0, nullptr);

  i2s_pin_config_t pins = {};
  pins.mck_io_num = I2S_PIN_NO_CHANGE;
  pins.bck_io_num = PIN_SPK_BCLK;
  pins.ws_io_num  = PIN_SPK_LRC;
  pins.data_out_num = PIN_SPK_DIN;
  pins.data_in_num  = I2S_PIN_NO_CHANGE;
  i2s_set_pin(I2S_NUM_1, &pins);
  i2s_zero_dma_buffer(I2S_NUM_1);
  spkRate = SR;
}

void speakerRate(uint32_t sr) {
  if (sr == 0 || sr == spkRate) return;
  i2s_set_clk(I2S_NUM_1, sr, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
  spkRate = sr;
}

// --------------------------- WAV -------------------------------------------
void putLE32(uint8_t *p, uint32_t v) { p[0]=v; p[1]=v>>8; p[2]=v>>16; p[3]=v>>24; }
void putLE16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }

void wavHeader(uint8_t *h, uint32_t dataBytes, uint32_t sr, uint16_t ch, uint16_t bits) {
  memcpy(h, "RIFF", 4);              putLE32(h + 4, 36 + dataBytes);
  memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4);         putLE32(h + 16, 16);
  putLE16(h + 20, 1);                putLE16(h + 22, ch);
  putLE32(h + 24, sr);               putLE32(h + 28, sr * ch * bits / 8);
  putLE16(h + 32, ch * bits / 8);    putLE16(h + 34, bits);
  memcpy(h + 36, "data", 4);         putLE32(h + 40, dataBytes);
}

// --------------------------- base64 ----------------------------------------
int b64val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;                                    // сюда попадают и переносы строк
}

// Декодирует в свежий PSRAM-буфер. Переносы строк и пробелы игнорируются —
// сервер имеет право отдать base64 в столбик.
uint8_t *b64decode(const char *src, size_t len, size_t &outLen) {
  outLen = 0;
  uint8_t *buf = (uint8_t *)psAlloc(len / 4 * 3 + 4);
  if (!buf) return nullptr;

  int acc = 0, bits = 0;
  size_t o = 0;
  for (size_t i = 0; i < len; i++) {
    const char c = src[i];
    if (c == '=') break;
    const int v = b64val(c);
    if (v < 0) continue;
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) { bits -= 8; buf[o++] = (uint8_t)(acc >> bits); }
  }
  outLen = o;
  if (o == 0) { free(buf); return nullptr; }
  return buf;
}

// --------------------------- Wi-Fi -----------------------------------------
bool connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;
  stage = "wifi";
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(200);
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[wifi] не подключился");
    return false;
  }
  Serial.print("[wifi] IP: ");
  Serial.println(WiFi.localIP());
  return true;
}

// --------------------------- запись ----------------------------------------
// Пишет, пока держат кнопку. Возвращает готовый WAV в PSRAM.
uint8_t *record(size_t &wavLen) {
  wavLen = 0;
  stage = "запись";
  i2s_zero_dma_buffer(I2S_NUM_0);

  constexpr int BATCH = 256;
  static int32_t raw[BATCH];

  size_t got = 0;
  const uint32_t t0 = millis();

  while (digitalRead(PIN_TOUCH) == HIGH) {
    if (got >= MAX_SAMPLES) break;
    if (millis() - t0 >= (uint32_t)AUDIO_MAX_SEC * 1000UL) break;

    size_t bytes = 0;
    if (i2s_read(I2S_NUM_0, raw, sizeof(raw), &bytes, pdMS_TO_TICKS(40)) != ESP_OK) continue;

    const size_t n = bytes / sizeof(int32_t);
    for (size_t i = 0; i < n && got < MAX_SAMPLES; i++) {
      // INMP441: значащие 24 бита в старших разрядах слова, нам нужны верхние 16.
      pcmBuf[got++] = (int16_t)(raw[i] >> 16);
    }
  }

  Serial.printf("[запись] %u сэмплов, %.2f с\n", (unsigned)got, (millis() - t0) / 1000.f);
  if (got < MIN_SAMPLES) return nullptr;

  const size_t dataBytes = got * 2;
  wavLen = 44 + dataBytes;
  uint8_t *wav = (uint8_t *)psAlloc(wavLen);
  if (!wav) { wavLen = 0; return nullptr; }
  wavHeader(wav, dataBytes, SR, 1, 16);
  memcpy(wav + 44, pcmBuf, dataBytes);
  return wav;
}

// --------------------------- запрос ----------------------------------------
// Тело ответа целиком в PSRAM, с запасным нулём в конце — чтобы JSON можно
// было разобрать прямо по нему, без ещё одной копии.
uint8_t *post(const uint8_t *wav, size_t wavLen, size_t &respLen) {
  respLen = 0;
  if (!connectWiFi()) return nullptr;
  stage = "запрос";

  WiFiClientSecure client;
  client.setInsecure();                  // сервер на sslip.io, своего CA у него нет

  HTTPClient http;
  // Не больше 65535: HTTPClient::setTimeout принимает uint16_t, и 120000 из
  // прошивки Сергея молча превращались в 54 секунды. Минута ожидания честная,
  // а от зависшего соединения нас всё равно страхует свой таймаут ниже.
  http.setTimeout(60000);
  if (!http.begin(client, String(API_HOST) + API_PATH)) return nullptr;

  const String boundary = "----Yasmina7d3a";
  http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
  http.addHeader("Accept", "audio/wav, application/json, */*");

  String head = "--" + boundary + "\r\n"
                "Content-Disposition: form-data; name=\"audio\"; filename=\"rec.wav\"\r\n"
                "Content-Type: application/octet-stream\r\n\r\n";
  String tail;
  auto field = [&](const char *name, const String &value) {
    if (!value.length()) return;
    tail += "\r\n--" + boundary + "\r\n";
    tail += "Content-Disposition: form-data; name=\"" + String(name) + "\"\r\n\r\n";
    tail += value;
  };
  field("session_id", sessionId);
  field("speaker", API_SPEAKER);
  field("output_format", API_FORMAT);
  tail += "\r\n--" + boundary + "--\r\n";

  const size_t total = head.length() + wavLen + tail.length();
  uint8_t *body = (uint8_t *)psAlloc(total);
  if (!body) { http.end(); return nullptr; }
  memcpy(body, head.c_str(), head.length());
  memcpy(body + head.length(), wav, wavLen);
  memcpy(body + head.length() + wavLen, tail.c_str(), tail.length());

  Serial.printf("[http] POST %s, тело %u байт\n", API_PATH, (unsigned)total);
  const int code = http.POST(body, total);
  free(body);

  if (code <= 0) {
    Serial.printf("[http] ошибка: %s\n", http.errorToString(code).c_str());
    http.end();
    return nullptr;
  }

  const int len = http.getSize();
  size_t cap = (len > 0) ? (size_t)len : 256 * 1024;
  uint8_t *buf = (uint8_t *)psAlloc(cap + 1);
  if (!buf) { http.end(); return nullptr; }

  WiFiClient *stream = http.getStreamPtr();
  size_t got = 0;
  uint32_t idle = millis();
  while ((len < 0 || got < (size_t)len) && (http.connected() || stream->available())) {
    const size_t avail = stream->available();
    if (!avail) {
      if (millis() - idle > 15000) break;
      delay(2);
      continue;
    }
    if (got + avail > cap) {                       // ответ длиннее заявленного — растём
      const size_t grow = cap * 2 + avail;
      uint8_t *bigger = (uint8_t *)psAlloc(grow + 1);
      if (!bigger) break;
      memcpy(bigger, buf, got);
      free(buf);
      buf = bigger;
      cap = grow;
    }
    got += stream->readBytes(buf + got, avail);
    idle = millis();
  }
  http.end();

  buf[got] = 0;
  respLen = got;
  Serial.printf("[http] %d, получено %u байт\n", code, (unsigned)got);
  if (got == 0) { free(buf); return nullptr; }
  return buf;
}

// --------------------------- эмоция ----------------------------------------
// Сначала смотрим, что сказал сервер: если в ответе есть emotion/mood — верим
// ему. Если поля нет (старый сервер Сергея его не шлёт), угадываем по тексту
// ответа. Промах здесь не страшен: хуже нейтрального лица не будет.
struct Keys { FaceState st; const char *words; };

const Keys KEYS[] = {
  { FACE_LOVE,     "яратам|сөям|люблю|любовь|сердеч|милый|милая|родн" },
  { FACE_HAPPY,    "шат|бәйрәм|котлыйм|ура|рад|здорово|отлично|прекрасн|поздравля|хорош|молодец|ха-ха" },
  { FACE_WINK,     "шаян|шаярт|шучу|шутка|подмигн|секрет|между нами" },
  { FACE_SURPRISE, "гаҗәп|ничек|вот это|ого|надо же|неужели|удивит|серьёзно" },
  { FACE_SAD,      "кызганыч|моңсу|жаль|извини|прости|грустн|к сожалению|не получилось|ошибк" },
  { FACE_SLEEP,    "йокы|спокойной ночи|сау бул|до свидания|пока-пока" },
};

FaceState stateFromName(const char *name) {
  if (!name || !*name) return FACE_STATE_COUNT;
  const String n = String(name);
  if (n.indexOf("love") >= 0 || n.indexOf("heart") >= 0)   return FACE_LOVE;
  if (n.indexOf("happy") >= 0 || n.indexOf("joy") >= 0 ||
      n.indexOf("glad") >= 0)                              return FACE_HAPPY;
  if (n.indexOf("wink") >= 0 || n.indexOf("joke") >= 0 ||
      n.indexOf("play") >= 0)                              return FACE_WINK;
  if (n.indexOf("surprise") >= 0 || n.indexOf("wow") >= 0) return FACE_SURPRISE;
  if (n.indexOf("sad") >= 0 || n.indexOf("sorry") >= 0)    return FACE_SAD;
  if (n.indexOf("sleep") >= 0 || n.indexOf("tired") >= 0)  return FACE_SLEEP;
  if (n.indexOf("neutral") >= 0 || n.indexOf("calm") >= 0) return FACE_SPEAK;
  return FACE_STATE_COUNT;
}

// Ищем слово в тексте без учёта регистра. Русский и татарский в UTF-8 —
// двухбайтовые, toLowerCase() их не трогает, поэтому в словарях выше лежат
// только строчные формы и корни слов: сравнение идёт по подстроке.
bool hasAny(const String &textLower, const char *words) {
  const String list(words);
  int from = 0;
  while (from <= list.length()) {
    int bar = list.indexOf('|', from);
    if (bar < 0) bar = list.length();
    const String w = list.substring(from, bar);
    if (w.length() && textLower.indexOf(w) >= 0) return true;
    from = bar + 1;
  }
  return false;
}

FaceState guessEmotion(const String &reply) {
  String t = reply;
  t.toLowerCase();                                // работает по латинице, кириллице не мешает
  for (const Keys &k : KEYS)
    if (hasAny(t, k.words)) return k.st;
  return FACE_SPEAK;                              // нейтральное говорящее лицо
}

// --------------------------- воспроизведение --------------------------------
// Пока играем — ведём рот по громкости самого звука: раскрытие считается по
// RMS чанка, поэтому губы совпадают с речью, а не болтаются сами по себе.
bool play(const uint8_t *bin, size_t len) {
  const uint8_t *pcm = bin;
  size_t pcmBytes = len;
  uint16_t channels = 1, bits = 16;
  uint32_t sr = SR;

  if (len >= 44 && memcmp(bin, "RIFF", 4) == 0 && memcmp(bin + 8, "WAVE", 4) == 0) {
    size_t pos = 12;
    while (pos + 8 <= len) {
      const uint32_t csize = (uint32_t)bin[pos+4] | ((uint32_t)bin[pos+5] << 8)
                           | ((uint32_t)bin[pos+6] << 16) | ((uint32_t)bin[pos+7] << 24);
      if (memcmp(bin + pos, "fmt ", 4) == 0 && pos + 24 <= len) {
        const uint8_t *f = bin + pos + 8;
        channels = f[2] | (f[3] << 8);
        sr       = f[4] | (f[5] << 8) | (f[6] << 16) | (f[7] << 24);
        bits     = f[14] | (f[15] << 8);
      } else if (memcmp(bin + pos, "data", 4) == 0) {
        pcm = bin + pos + 8;
        pcmBytes = len - (pos + 8);
        if (csize && pcmBytes > csize) pcmBytes = csize;
        break;
      }
      pos += 8 + csize + (csize & 1);
    }
  }

  Serial.printf("[звук] %u канал(ов), %u бит, %u Гц, %u байт\n",
                channels, bits, (unsigned)sr, (unsigned)pcmBytes);
  if (bits != 16 || pcmBytes < 2) return false;
  speakerRate(sr);

  constexpr int CHUNK = 256;
  int16_t out[CHUNK];
  const int16_t *src = (const int16_t *)pcm;
  const int frames = (int)(pcmBytes / 2) / channels;

  for (int i = 0; i < frames; ) {
    const int n = min(CHUNK, frames - i);
    if (channels == 2) {
      for (int k = 0; k < n; k++) out[k] = src[(i + k) * 2];   // берём левый канал
    } else {
      memcpy(out, src + i, n * sizeof(int16_t));
    }

    // Громкость чанка → раскрытие рта. Делитель подобран так, чтобы обычная
    // речь давала почти полное раскрытие, а не еле заметное шевеление.
    uint64_t sum = 0;
    for (int k = 0; k < n; k++) sum += (int32_t)out[k] * out[k];
    const float rms = sqrtf((float)sum / n);
    float level = rms / 5000.f;
    if (level > 1.f) level = 1.f;
    face::setSpeechLevel(level);

    size_t written = 0;
    i2s_write(I2S_NUM_1, out, n * sizeof(int16_t), &written, portMAX_DELAY);
    i += n;
  }

  memset(out, 0, sizeof(out));                    // хвост тишины, чтобы не щёлкнуло
  size_t written = 0;
  i2s_write(I2S_NUM_1, out, sizeof(out), &written, portMAX_DELAY);
  face::setSpeechLevel(-1.f);
  return true;
}

// --------------------------- разбор ответа ---------------------------------
bool handle(uint8_t *resp, size_t len) {
  // Сервер может ответить и голым WAV — тогда эмоции в ответе нет,
  // говорим с нейтральным лицом.
  if (len >= 12 && memcmp(resp, "RIFF", 4) == 0 && memcmp(resp + 8, "WAVE", 4) == 0) {
    face::setState(FACE_SPEAK);
    face::setSpeaking(true);
    const bool ok = play(resp, len);
    face::setSpeaking(false);
    return ok;
  }

  JsonDocument doc;
  // Zero-copy: строки остаются указателями внутрь resp, поэтому огромный
  // audio_base64 не копируется во внутреннюю кучу — там его не разместить.
  const DeserializationError err = deserializeJson(doc, (char *)resp);
  if (err) { Serial.printf("[json] %s\n", err.c_str()); return false; }

  const char *sid   = doc["session_id"]  | "";
  const char *tr    = doc["transcript"]  | "";
  const char *reply = doc["reply_text"]  | "";
  const char *emo   = doc["emotion"]     | (const char *)(doc["mood"] | "");
  const char *b64   = doc["audio_base64"]| "";
  if (*sid) sessionId = sid;

  Serial.printf("[ты] %s\n[ясмина] %s\n", tr, reply);

  FaceState st = stateFromName(emo);
  if (st == FACE_STATE_COUNT) st = guessEmotion(String(reply));
  Serial.printf("[лицо] %s%s\n", face::stateName(st), *emo ? " (от сервера)" : " (по тексту)");

  face::setState(st);
  face::setSpeaking(true);

  bool ok = false;
  if (*b64) {
    size_t binLen = 0;
    uint8_t *bin = b64decode(b64, strlen(b64), binLen);
    if (bin) {
      ok = play(bin, binLen);
      free(bin);
    }
  } else {
    // Голоса нет — хотя бы подержим эмоцию, чтобы ответ не пропал впустую.
    delay(1200);
    ok = *reply != 0;
  }
  face::setSpeaking(false);
  return ok;
}

} // namespace

// --------------------------- публичное -------------------------------------
void begin() {
  pinMode(PIN_TOUCH, INPUT);

  pcmBuf = (int16_t *)psAlloc(MAX_SAMPLES * sizeof(int16_t));
  if (!pcmBuf) {
    Serial.println("[голос] не хватило памяти под запись — нужна PSRAM");
    stage = "нет памяти";
    return;
  }

  initMic();
  initSpeaker();
  connectWiFi();
  lastActivityMs = millis();
  stage = "жду";
  Serial.println("[голос] готов, держи кнопку и говори");
}

const char *stageName() { return stage; }

void tick() {
  if (!pcmBuf) { delay(500); return; }

  if (digitalRead(PIN_TOUCH) != HIGH) {
    // Ничего не происходит: через полторы минуты тишины Ясмина засыпает,
    // касание её будит.
    if (face::state() != FACE_SLEEP && millis() - lastActivityMs > SLEEP_AFTER_MS) {
      face::setState(FACE_SLEEP);
      stage = "сон";
    }
    delay(10);
    return;
  }

  delay(30);                                      // дребезг сенсора
  if (digitalRead(PIN_TOUCH) != HIGH) return;

  lastActivityMs = millis();
  face::setState(FACE_LISTEN);                    // слушаю — пока держат кнопку

  size_t wavLen = 0;
  uint8_t *wav = record(wavLen);

  if (!wav) {                                     // мазнули по кнопке
    face::setState(FACE_IDLE);
    stage = "жду";
    lastActivityMs = millis();
    return;
  }

  // Кнопку отпустили — думаем. Эмоцию пока не трогаем: лицо спокойное,
  // а взгляд уходит вверх и плывёт.
  face::setState(FACE_IDLE);
  face::setThinking(true);
  stage = "думаю";

  size_t respLen = 0;
  uint8_t *resp = post(wav, wavLen, respLen);
  free(wav);

  face::setThinking(false);

  if (!resp) {
    face::setState(FACE_SAD);
    stage = "нет сети";
    delay(1800);
  } else {
    stage = "отвечаю";
    if (!handle(resp, respLen)) {
      face::setState(FACE_SAD);
      delay(1500);
    }
    free(resp);
  }

  face::setState(FACE_IDLE);
  stage = "жду";
  lastActivityMs = millis();
}

} // namespace voice
