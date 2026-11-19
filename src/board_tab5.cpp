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
#include <atomic>

// =====================================================================
//  Board
// =====================================================================
namespace Board {

static M5Canvas* s_canvas = nullptr;

const char* name() { return "Tab5"; }

bool beginDisplay() {
  M5.Display.setRotation(TAB5_ROTATION);
  M5.Display.fillScreen(TFT_BLACK);
  s_canvas = new M5Canvas(&M5.Display);
  s_canvas->setPsram(true);
  s_canvas->setColorDepth(16);
  if (!s_canvas->createSprite(M5.Display.width(), M5.Display.height())) return false;
  log_w("Tab5 display %dx%d", M5.Display.width(), M5.Display.height());
  return true;
}

LGFX_Sprite& canvas() { return *s_canvas; }
void present() { s_canvas->pushSprite(0, 0); }
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
