// M5Stack Tab5 (ESP32-P4): 5" 1280x720 MIPI-DSI panel, touch, ES8388 speaker output,
// battery via M5.Power - all through M5Unified / M5GFX. Wi-Fi is the on-board ESP32-C6
// (ESP-Hosted over SDIO); M5.begin() sets its pins so the normal WiFi library works.
#if defined(BOARD_TAB5)
#include "board.h"
#include "config.h"
#include "audio_out.h"
#include "battery.h"
#include "power.h"
#include "rtsp_audio.h"
#include <WiFi.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_heap_caps.h>
#include <esp_cache.h>
#include <lgfx/v1/platforms/esp32p4/Panel_DSI.hpp>
#include <atomic>

// =====================================================================
//  Board
// =====================================================================
namespace Board {

static M5Canvas* s_canvas = nullptr;
static uint16_t* s_fb = nullptr;          // the panel's frame buffer (native portrait order)
static size_t s_fbLen = 0;
static int s_nativeW = 0, s_nativeH = 0;

const char* name() { return "Tab5"; }

// Find where three marker pixels land in a native-order 16-bit buffer.
static bool findMarkers(const uint16_t* buf, int n, int pos[3]) {
  static const uint16_t marks[3] = {0x1234, 0x2345, 0x3456};
  for (int m = 0; m < 3; m++) {
    pos[m] = -1;
    for (int i = 0; i < n; i++) if (buf[i] == marks[m]) { pos[m] = i; break; }
    if (pos[m] < 0) return false;
  }
  return true;
}
template <typename T> static void drawMarkers(T& g) {
  g.drawPixel(0, 0, (uint16_t)0x1234);
  g.drawPixel(1, 0, (uint16_t)0x2345);
  g.drawPixel(0, 1, (uint16_t)0x3456);
}

bool beginDisplay() {
  M5.Display.setRotation(TAB5_ROTATION);
  M5.Display.fillScreen(TFT_BLACK);
  int w = M5.Display.width(), h = M5.Display.height();
  s_nativeW = min(w, h);                  // the panel is portrait: 720 x 1280
  s_nativeH = max(w, h);
  s_canvas = new M5Canvas(&M5.Display);
  s_canvas->setPsram(true);
  // Same pixel format as the screen buffer (RGB565, not byte-swapped).
  s_canvas->setColorDepth(M5.Display.getColorDepth());
  if (!s_canvas->createSprite(s_nativeW, s_nativeH)) return false;

  // Fast path: draw into a canvas laid out exactly like the panel's frame buffer, so an
  // update is a plain copy. Find the canvas rotation that matches the display's mapping.
  auto* dsi = (lgfx::Panel_DSI*)M5.Display.getPanel();
  s_fb = dsi ? (uint16_t*)dsi->config_detail().buffer : nullptr;
  s_fbLen = (size_t)s_nativeW * s_nativeH * 2;
  int want[3], got[3];
  bool fast = false;
  if (s_fb) {
    M5.Display.fillScreen(TFT_BLACK);
    drawMarkers(M5.Display);
    M5.Display.display();
    if (findMarkers(s_fb, s_nativeW * s_nativeH, want)) {
      for (int r = 0; r < 8 && !fast; r++) {
        s_canvas->setRotation(r);
        s_canvas->fillScreen(TFT_BLACK);
        drawMarkers(*s_canvas);
        if (findMarkers((const uint16_t*)s_canvas->getBuffer(), s_nativeW * s_nativeH, got) &&
            got[0] == want[0] && got[1] == want[1] && got[2] == want[2]) fast = true;
      }
    }
    M5.Display.fillScreen(TFT_BLACK);
  }
  if (!fast) {                            // fall back: landscape canvas, library copy (slow)
    s_fb = nullptr;
    s_canvas->deleteSprite();
    s_canvas->createSprite(w, h);
    log_w("Tab5: frame buffer mapping not found - using slow screen updates");
  }
  s_canvas->fillScreen(TFT_BLACK);
  log_w("Tab5 display %dx%d, canvas rotation %d, %s updates", s_canvas->width(), s_canvas->height(),
        s_canvas->getRotation(), s_fb ? "fast" : "slow");
  return true;
}

int frameWidth() { return s_fb ? s_nativeW : s_canvas->width(); }
int frameHeight() { return s_fb ? s_nativeH : s_canvas->height(); }
bool frameLittleEndian() { return true; }

LGFX_Sprite& canvas() { return *s_canvas; }
// Copy only the frame-buffer rows that changed since the last update. PSRAM runs at
// 80 MHz here and the display reads the same memory continuously; full-frame copies
// starve it ("lcd.dsi.dpi: underrun"). A small fingerprint per row finds the changes.
static uint32_t* s_rowHash = nullptr;

static uint32_t rowHash(const uint32_t* p, int words) {
  uint32_t h = 2166136261u;
  for (int i = 0; i < words; i++) h = (h ^ p[i]) * 16777619u;
  return h;
}

void present() {
  if (s_fb) {
    const int rowBytes = s_nativeW * 2, words = rowBytes / 4;
    if (!s_rowHash) {
      s_rowHash = (uint32_t*)heap_caps_calloc(s_nativeH, sizeof(uint32_t), MALLOC_CAP_INTERNAL);
      if (!s_rowHash) {                   // no memory for fingerprints: copy everything
        memcpy(s_fb, s_canvas->getBuffer(), s_fbLen);
        esp_cache_msync(s_fb, s_fbLen, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        return;
      }
      for (int y = 0; y < s_nativeH; y++) s_rowHash[y] = ~0u;   // force a first full copy
    }
    auto* src = (const uint8_t*)s_canvas->getBuffer();
    auto* dst = (uint8_t*)s_fb;
    int y = 0;
    while (y < s_nativeH) {
      uint32_t hsh = rowHash((const uint32_t*)(src + (size_t)y * rowBytes), words);
      if (hsh == s_rowHash[y]) { y++; continue; }
      int y0 = y;                         // copy a run of changed rows in one go
      do { s_rowHash[y] = hsh; y++; } while (y < s_nativeH &&
               (hsh = rowHash((const uint32_t*)(src + (size_t)y * rowBytes), words)) != s_rowHash[y]);
      size_t off = (size_t)y0 * rowBytes, len = (size_t)(y - y0) * rowBytes;
      memcpy(dst + off, src + off, len);
      esp_cache_msync(dst + off, len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    }
  } else {
    s_canvas->pushSprite(0, 0);
  }
}
void setBrightness(int pct) { M5.Display.setBrightness(constrain(pct, 0, 100) * 255 / 100); }

bool touch(int& x, int& y) {
  M5.update();
  auto d = M5.Touch.getDetail();
  if (!d.isPressed()) return false;
  x = d.x;
  y = d.y;
  return true;
}

bool hasBootButton() { return false; }

static const esp_partition_t* factoryPartition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
}

// Running from an app slot with a separate factory app (the Launcher) present.
bool hasLauncher() {
  const esp_partition_t* f = factoryPartition();
  return f && esp_ota_get_running_partition() != f;
}

void exitToLauncher() {
  const esp_partition_t* f = factoryPartition();
  if (f) esp_ota_set_boot_partition(f);
  ESP.restart();
}

}  // namespace Board

// =====================================================================
//  Speaker: same jitter buffer as the FNK0104N, played through M5.Speaker
// =====================================================================
namespace AudioOut {

static const size_t RING = 32768;                // samples, power of two
static const int CH = 0;                         // M5.Speaker virtual channel for the stream
static const size_t CHUNK = 1024;                // samples per playRaw() buffer
static int16_t* s_ring = nullptr;
static std::atomic<uint32_t> s_head{0}, s_tail{0};
static std::atomic<uint32_t> s_rate{8000};
static std::atomic<bool> s_flush{false};
static std::atomic<uint32_t> s_lastAudioMs{0};
static int s_volume = SPEAKER_VOLUME_DEFAULT;
static bool s_muted = false;

void push(const int16_t* samples, size_t n) {
  if (!s_ring) return;
  uint32_t h = s_head.load(std::memory_order_relaxed);
  uint32_t t = s_tail.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; i++) {
    if (h - t >= RING - 1) break;
    s_ring[h & (RING - 1)] = samples[i];
    h++;
  }
  s_head.store(h, std::memory_order_release);
  s_lastAudioMs.store(millis());
}

void flush() { s_flush = true; }
void setStreamRate(uint32_t hz) { if (hz >= 4000 && hz <= 48000) s_rate = hz; }
uint32_t streamRate() { return s_rate.load(); }
uint32_t queued() { return s_head.load() - s_tail.load(); }
bool playing() { return millis() - s_lastAudioMs.load() < 1500; }

static void applyVolume() { M5.Speaker.setVolume(s_muted ? 0 : s_volume * 255 / 100); }
void setVolume(int pct) { s_volume = constrain(pct, 0, 100); applyVolume(); }
int volume() { return s_volume; }
void setMuted(bool m) { s_muted = m; applyVolume(); }
bool muted() { return s_muted; }

void beep(int hz, int ms) { M5.Speaker.tone(hz, ms, 7); }

static void audioTask(void*) {
  // Three buffers: one playing, one queued in M5.Speaker, one being filled.
  static int16_t* bufs[3];
  for (auto& b : bufs) b = (int16_t*)heap_caps_malloc(CHUNK * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  int bi = 0;
  bool primed = false;
  for (;;) {
    if (s_flush.exchange(false)) {
      s_tail.store(s_head.load());
      primed = false;
      M5.Speaker.stop(CH);
    }
    uint32_t rate = s_rate.load();
    uint32_t avail = s_head.load(std::memory_order_acquire) - s_tail.load();
    uint32_t prebuf = rate * AUDIO_PREBUFFER_MS / 1000;
    if (!primed && avail >= prebuf) primed = true;
    if (primed && avail > prebuf * 4) { s_tail.fetch_add(avail - prebuf); avail = prebuf; }   // catch up
    if (!primed || avail == 0) {
      if (avail == 0) primed = false;
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    if (M5.Speaker.isPlaying(CH) >= 2) { vTaskDelay(pdMS_TO_TICKS(3)); continue; }   // queue full
    size_t n = min<size_t>(avail, CHUNK);
    uint32_t t = s_tail.load();
    for (size_t i = 0; i < n; i++) bufs[bi][i] = s_ring[(t + i) & (RING - 1)];
    s_tail.store(t + n, std::memory_order_release);
    M5.Speaker.playRaw(bufs[bi], n, rate, false, 1, CH, false);
    bi = (bi + 1) % 3;
  }
}

bool begin() {
  s_ring = (int16_t*)heap_caps_malloc(RING * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  if (!s_ring) return false;
  if (!M5.Speaker.isEnabled()) M5.Speaker.begin();
  applyVolume();
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, 20, nullptr, 0);
  return true;
}

}  // namespace AudioOut

// =====================================================================
//  Battery (NP-F550 pack on the Tab5 kit; none on the bare Tab5)
// =====================================================================
namespace Battery {

static int s_mv = 0, s_pct = -1;
static uint32_t s_last = 0;

void begin() { s_last = 0; }
void loop() {
  if (s_last && millis() - s_last < 1000) return;
  s_last = millis() | 1;
  s_mv = M5.Power.getBatteryVoltage();
  s_pct = s_mv < 5000 ? -1 : constrain((int)M5.Power.getBatteryLevel(), 0, 100);   // 2S pack: < 5 V = no battery
#if BATTERY_DEBUG
  Serial.printf("battery %d mV  %d%%\n", s_mv, s_pct);
#endif
}
int millivolts() { return s_mv; }
int percent() { return s_pct; }

}  // namespace Battery

// =====================================================================
//  Power: no BOOT button to read; the power menu is opened from KEYS > POWER
// =====================================================================
namespace Power {

void begin() {}
bool bootHeld() { return false; }
bool bootPressed() { return false; }

static void quiet() {
  RtspAudio::shutdown();          // TEARDOWN so the scanner frees its single audio slot
  AudioOut::setMuted(true);
  WiFi.disconnect(true);
}

void restart() {
  quiet();
  Serial.flush();
  delay(100);
  ESP.restart();
}

void off() {
  quiet();
  Board::setBrightness(0);
  Serial.println("Powering off");
  Serial.flush();
  M5.Power.powerOff();
}

}  // namespace Power
#endif  // BOARD_TAB5
