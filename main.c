/*
 * ESP32-S3 N16R8 + INMP441 + MAX98357 + GC9A01 (SPI) + сенсорная кнопка
 * Tatarser OLD server: https://tatarser.213-109-202-195.sslip.io
 *
 * ВАЖНО: включите PSRAM (OPI PSRAM). Плата: ESP32S3 Dev Module.
 *        Библиотеки: Adafruit GC9A01A, Adafruit GFX, ArduinoJson
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_GC9A01A.h>
#include <SPI.h>
#include "driver/i2s.h"
#include "esp_heap_caps.h"

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

// ================= НАСТРОЙКИ =================
const char* WIFI_SSID = "SSID";
const char* WIFI_PASS = "PASS";

const char* API_HOST = "https://tatarser.213-109-202-195.sslip.io";
const char* API_PATH = "/v1/dialog/audio";

const char* SPEAKER       = "alsu";
const char* OUTPUT_FORMAT = "wav";

// ================= ЭКРАН GC9A01 (SPI) =================
#define TFT_CS    10
#define TFT_DC    11
#define TFT_RST   12
#define TFT_SCLK  9
#define TFT_MOSI  8

Adafruit_GC9A01A tft(TFT_CS, TFT_DC, TFT_MOSI, TFT_SCLK, TFT_RST);

// ================= СЕНСОРНАЯ КНОПКА =================
#define TOUCH_PIN 4

// ================= I2S ПИНЫ =================
#define I2S_MIC_PORT     I2S_NUM_0
#define MIC_SCK_PIN      5
#define MIC_WS_PIN       6
#define MIC_SD_PIN       7

#define I2S_SPK_PORT     I2S_NUM_1
#define SPK_BCLK_PIN     15
#define SPK_LRC_PIN      16
#define SPK_DIN_PIN      17

// ================= АУДИО =================
#define SAMPLE_RATE      16000
#define MAX_LISTEN_SEC   20
#define MAX_SAMPLES      (SAMPLE_RATE * MAX_LISTEN_SEC)
#define MAX_PCM_BYTES    (MAX_SAMPLES * 2)

static int16_t *pcmBuf = nullptr;
String g_sessionId = "";

// ================= ЭКРАН =================
String toDisplayable(const String &s, size_t maxLen) {
    String out;
    out.reserve(maxLen);
    for (size_t i = 0; i < s.length() && out.length() < maxLen; i++) {
        char c = s[i];
        if (c >= 32 && c <= 126) {
            out += c;
        } else if ((uint8_t)c >= 0x80) {
            if (out.length() == 0 || out[out.length() - 1] != '*') out += '*';
        }
    }
    return out;
}

void tftMsg(const String &l1, const String &l2 = "", const String &l3 = "") {
    tft.fillScreen(GC9A01A_BLACK);
    tft.setTextSize(2);
    tft.setTextColor(GC9A01A_WHITE);
    tft.setCursor(20, 80);
    tft.println(toDisplayable(l1, 16));
    if (l2.length()) {
        tft.setCursor(20, 110);
        tft.println(toDisplayable(l2, 16));
    }
    if (l3.length()) {
        tft.setCursor(20, 140);
        tft.println(toDisplayable(l3, 16));
    }
}

// ================= I2S =================
void initI2SMic() {
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };
    i2s_driver_install(I2S_MIC_PORT, &cfg, 0, NULL);
    i2s_pin_config_t pins = {
        .mck_io_num   = I2S_PIN_NO_CHANGE,
        .bck_io_num   = MIC_SCK_PIN,
        .ws_io_num    = MIC_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = MIC_SD_PIN
    };
    i2s_set_pin(I2S_MIC_PORT, &pins);
    i2s_zero_dma_buffer(I2S_MIC_PORT);
}

void initI2SSpeaker(uint32_t sr = SAMPLE_RATE) {
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = sr,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };
    i2s_driver_uninstall(I2S_SPK_PORT);
    i2s_driver_install(I2S_SPK_PORT, &cfg, 0, NULL);
    i2s_pin_config_t pins = {
        .mck_io_num   = I2S_PIN_NO_CHANGE,
        .bck_io_num   = SPK_BCLK_PIN,
        .ws_io_num    = SPK_LRC_PIN,
        .data_out_num = SPK_DIN_PIN,
        .data_in_num  = I2S_PIN_NO_CHANGE
    };
    i2s_set_pin(I2S_SPK_PORT, &pins);
    i2s_zero_dma_buffer(I2S_SPK_PORT);
}

// ================= base64 decode =================
int base64Decode(const String &b64, uint8_t **outBuf) {
    size_t n = b64.length();
    if (n == 0 || (n & 3) != 0) return -1;
    size_t outLen = (n / 4) * 3;
    if (b64[n - 1] == '=') outLen--;
    if (b64[n - 2] == '=') outLen--;
    uint8_t *buf = (uint8_t*) heap_caps_malloc(outLen, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = (uint8_t*) malloc(outLen);
    if (!buf) return -1;
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    size_t oi = 0;
    for (size_t i = 0; i < n; i += 4) {
        int a = val(b64[i]);
        int b = val(b64[i + 1]);
        int c = (b64[i + 2] == '=') ? -2 : val(b64[i + 2]);
        int d = (b64[i + 3] == '=') ? -2 : val(b64[i + 3]);
        if (a < 0 || b < 0) { free(buf); return -1; }
        buf[oi++] = (uint8_t)((a << 2) | (b >> 4));
        if (c >= 0) buf[oi++] = (uint8_t)(((b & 0x0F) << 4) | (c >> 2));
        if (d >= 0) buf[oi++] = (uint8_t)(((c & 0x03) << 6) | d);
    }
    *outBuf = buf;
    return (int)outLen;
}

// ================= WAV =================
static void putLE32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
static void putLE16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
}
void writeWavHeader(uint8_t *h, uint32_t dataBytes,
                    uint32_t sampleRate, uint16_t channels, uint16_t bits) {
    memcpy(h, "RIFF", 4);
    putLE32(h + 4, 36 + dataBytes);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    putLE32(h + 16, 16);
    putLE16(h + 20, 1);
    putLE16(h + 22, channels);
    putLE32(h + 24, sampleRate);
    putLE32(h + 28, sampleRate * channels * bits / 8);
    putLE16(h + 32, channels * bits / 8);
    putLE16(h + 34, bits);
    memcpy(h + 36, "data", 4);
    putLE32(h + 40, dataBytes);
}

// ================= LISTEN =================
uint8_t* listenRaw(size_t &wavLenOut) {
    wavLenOut = 0;
    Serial.println("[listen] Жду касания...");
    tftMsg("Kosnis", "i govori");

    while (digitalRead(TOUCH_PIN) == LOW) delay(10);
    delay(30);
    if (digitalRead(TOUCH_PIN) == LOW) return nullptr;

    Serial.println("[listen] Пишу...");
    tftMsg("REC...", "Govori");
    i2s_zero_dma_buffer(I2S_MIC_PORT);

    size_t collected = 0;
    unsigned long tStart = millis();

    while (digitalRead(TOUCH_PIN) == HIGH) {
        if (collected >= MAX_SAMPLES) break;
        if (millis() - tStart >= (unsigned long)MAX_LISTEN_SEC * 1000UL) break;

        int32_t raw = 0;
        size_t bytesRead = 0;
        esp_err_t r = i2s_read(I2S_MIC_PORT, &raw, sizeof(raw),
                               &bytesRead, pdMS_TO_TICKS(20));
        if (r == ESP_OK && bytesRead == sizeof(raw)) {
            int32_t s24 = raw >> 8;
            if (s24 >  8388607) s24 =  8388607;
            if (s24 < -8388608) s24 = -8388608;
            pcmBuf[collected++] = (int16_t)(s24 >> 8);
        }
    }

    Serial.printf("[listen] %u сэмплов (%.2f сек)\n",
                  (unsigned)collected, (millis() - tStart) / 1000.0f);

    if (collected < SAMPLE_RATE / 4) {
        Serial.println("[listen] Слишком коротко");
        tftMsg("Slishkom korotko");
        return nullptr;
    }

    size_t dataBytes = collected * 2;
    wavLenOut = 44 + dataBytes;
    uint8_t *wav = (uint8_t*) heap_caps_malloc(wavLenOut, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!wav) wav = (uint8_t*) malloc(wavLenOut);
    if (!wav) {
        Serial.println("[listen] Нет памяти");
        wavLenOut = 0;
        return nullptr;
    }
    writeWavHeader(wav, dataBytes, SAMPLE_RATE, 1, 16);
    memcpy(wav + 44, pcmBuf, dataBytes);
    Serial.printf("[listen] WAV %u байт\n", (unsigned)wavLenOut);
    return wav;
}

// ================= PLAY =================
bool playWavBytes(const uint8_t *bin, size_t binLen) {
    const uint8_t *pcm = bin;
    size_t pcmBytes = binLen;
    uint16_t channels = 1;
    uint16_t bits = 16;
    uint32_t sampleRate = SAMPLE_RATE;

    if (binLen >= 44 && memcmp(bin, "RIFF", 4) == 0 && memcmp(bin + 8, "WAVE", 4) == 0) {
        size_t pos = 12;
        while (pos + 8 <= binLen) {
            uint32_t csize = (uint32_t)bin[pos + 4]
                           | ((uint32_t)bin[pos + 5] << 8)
                           | ((uint32_t)bin[pos + 6] << 16)
                           | ((uint32_t)bin[pos + 7] << 24);
            if (memcmp(bin + pos, "fmt ", 4) == 0 && pos + 8 + 16 <= binLen) {
                const uint8_t *f = bin + pos + 8;
                channels   = f[2]  | (f[3]  << 8);
                sampleRate = f[4]  | (f[5]  << 8) | (f[6]  << 16) | (f[7] << 24);
                bits       = f[14] | (f[15] << 8);
            } else if (memcmp(bin + pos, "data", 4) == 0) {
                pcm = bin + pos + 8;
                pcmBytes = binLen - (pos + 8);
                if ((uint32_t)pcmBytes > csize) pcmBytes = csize;
                break;
            }
            pos += 8 + csize + (csize & 1);
        }
    }

    Serial.printf("[play] WAV: %u ch, %u bit, %u Hz, %u байт\n",
                  channels, bits, sampleRate, (unsigned)pcmBytes);

    if (bits != 16) {
        Serial.println("[play] Только 16-bit PCM");
        return false;
    }

    if (sampleRate != 0 && sampleRate != SAMPLE_RATE) {
        initI2SSpeaker(sampleRate);
    }

    const size_t CHUNK_SAMPLES = 256;
    int16_t outBuf[CHUNK_SAMPLES];

    if (channels == 2) {
        const int16_t *src = (const int16_t*)pcm;
        int frames = (pcmBytes / 2) / 2;
        int i = 0;
        while (i < frames) {
            int n = min((int)CHUNK_SAMPLES, frames - i);
            for (int k = 0; k < n; k++) outBuf[k] = src[(i + k) * 2];
            size_t written = 0;
            i2s_write(I2S_SPK_PORT, outBuf, n * sizeof(int16_t), &written, portMAX_DELAY);
            i += n;
        }
    } else {
        const int16_t *src = (const int16_t*)pcm;
        int total = pcmBytes / 2;
        int i = 0;
        while (i < total) {
            int n = min((int)CHUNK_SAMPLES, total - i);
            memcpy(outBuf, src + i, n * sizeof(int16_t));
            size_t written = 0;
            i2s_write(I2S_SPK_PORT, outBuf, n * sizeof(int16_t), &written, portMAX_DELAY);
            i += n;
        }
    }

    memset(outBuf, 0, sizeof(outBuf));
    size_t written = 0;
    i2s_write(I2S_SPK_PORT, outBuf, sizeof(outBuf), &written, portMAX_DELAY);
    Serial.println("[play] OK");
    return true;
}

// ================= Wi-Fi =================
bool connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return true;
    tftMsg("WiFi...", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
        delay(300);
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.print("[WiFi] IP: ");
        Serial.println(WiFi.localIP());
        return true;
    }
    return false;
}

// ================= HTTP POST =================
String postDialogAudio(const uint8_t *wav, size_t wavLen) {
    if (!connectWiFi()) return "";

    WiFiClientSecure client;
    client.setInsecure();

    HTTPClient http;
    http.setTimeout(120000);
    if (!http.begin(client, String(API_HOST) + API_PATH)) return "";

    String boundary = "----ESP32Boundary7d3a";
    http.addHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http.addHeader("Accept", "audio/wav, application/json, */*");

    String head;
    head += "--" + boundary + "\r\n";
    head += "Content-Disposition: form-data; name=\"audio\"; filename=\"rec.wav\"\r\n";
    head += "Content-Type: application/octet-stream\r\n\r\n";

    String tail;
    if (g_sessionId.length()) {
        tail += "\r\n--" + boundary + "\r\n";
        tail += "Content-Disposition: form-data; name=\"session_id\"\r\n\r\n";
        tail += g_sessionId;
    }
    if (strlen(SPEAKER) > 0) {
        tail += "\r\n--" + boundary + "\r\n";
        tail += "Content-Disposition: form-data; name=\"speaker\"\r\n\r\n";
        tail += SPEAKER;
    }
    if (strlen(OUTPUT_FORMAT) > 0) {
        tail += "\r\n--" + boundary + "\r\n";
        tail += "Content-Disposition: form-data; name=\"output_format\"\r\n\r\n";
        tail += OUTPUT_FORMAT;
    }
    tail += "\r\n--" + boundary + "--\r\n";

    size_t total = head.length() + wavLen + tail.length();
    uint8_t *body = (uint8_t*) heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!body) body = (uint8_t*) malloc(total);
    if (!body) { http.end(); return ""; }

    memcpy(body,                          head.c_str(), head.length());
    memcpy(body + head.length(),          wav,          wavLen);
    memcpy(body + head.length() + wavLen, tail.c_str(), tail.length());

    Serial.printf("[HTTP] POST %s (total=%u, wav=%u)\n",
                  API_PATH, (unsigned)total, (unsigned)wavLen);
    tftMsg("Otpravka...", "Zhdem otvet");

    int code = http.POST(body, total);
    free(body);

    String resp;
    if (code > 0) {
        Serial.printf("[HTTP] %d, body=%u байт\n", code, http.getSize());
        resp = http.getString();
    } else {
        Serial.printf("[HTTP] err: %s\n", http.errorToString(code).c_str());
    }
    http.end();
    return resp;
}

// ================= Разбор ответа =================
bool parseAndPlayJson(const String &json) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) { Serial.printf("[JSON] %s\n", err.c_str()); return false; }

    String sid  = doc["session_id"]   | "";
    String tr   = doc["transcript"]   | "";
    String rp   = doc["reply_text"]   | "";
    String ab64 = doc["audio_base64"] | "";
    if (sid.length()) g_sessionId = sid;

    tftMsg("Ty:", toDisplayable(tr, 16));
    delay(1500);
    tftMsg("Otvet:", toDisplayable(rp, 16));

    bool ok = false;
    if (ab64.length()) {
        uint8_t *bin = nullptr;
        int binLen = base64Decode(ab64, &bin);
        if (binLen > 0 && bin) {
            ok = playWavBytes(bin, binLen);
            free(bin);
        }
    }
    return ok;
}

bool handleResponse(const String &resp) {
    if (resp.length() >= 12 &&
        memcmp(resp.c_str(), "RIFF", 4) == 0 &&
        memcmp(resp.c_str() + 8, "WAVE", 4) == 0) {
        Serial.println("[resp] Бинарный WAV");
        tftMsg("Otvet WAV", "Igraem...");
        return playWavBytes((const uint8_t*)resp.c_str(), resp.length());
    }
    Serial.println("[resp] JSON");
    return parseAndPlayJson(resp);
}

// ================= SETUP / LOOP =================
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== Start ===");

    pinMode(TOUCH_PIN, INPUT);
    Serial.println("[1] pinMode OK");

    SPI.begin(TFT_SCLK, -1, TFT_MOSI, TFT_CS);
    Serial.println("[2] SPI.begin OK");

    tft.begin();
    Serial.println("[3] tft.begin OK");

    tft.setRotation(0);
    tft.fillScreen(GC9A01A_BLACK);
    tftMsg("Tatarser", "Boot...");
    Serial.println("[4] tft draw OK");

    pcmBuf = (int16_t*) heap_caps_malloc(MAX_PCM_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcmBuf) pcmBuf = (int16_t*) malloc(MAX_PCM_BYTES);
    if (!pcmBuf) {
        Serial.println("[!] Нет памяти под pcmBuf");
        tftMsg("No PSRAM!");
        while (true) delay(1000);
    }
    Serial.println("[5] pcmBuf OK");

    initI2SMic();
    Serial.println("[6] mic OK");
    initI2SSpeaker();
    Serial.println("[7] spk OK");

    connectWiFi();
    Serial.println("[8] wifi OK");

    tftMsg("Gotov", "Kosnis knopki");
    Serial.println("[9] ready");
}

void loop() {
    size_t wavLen = 0;
    uint8_t *wav = listenRaw(wavLen);
    if (!wav || wavLen == 0) { delay(500); return; }

    tftMsg("Otpravka...", "Zhdem otvet");
    String resp = postDialogAudio(wav, wavLen);
    free(wav);

    if (resp.length() == 0) {
        tftMsg("Oshibka seti", "Povtori");
        delay(1500);
        return;
    }

    if (!handleResponse(resp)) {
        tftMsg("Oshibka otveta");
        delay(1500);
        return;
    }

    tftMsg("Gotov", "Kosnis knopki");
    delay(200);
}
