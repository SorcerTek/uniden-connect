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
#include <rom/ets_sys.h>
#include <esp_async_memcpy.h>
#include <freertos/semphr.h>
#include <lgfx/v1/platforms/esp32p4/Panel_DSI.hpp>
#include <atomic>

namespace AudioOut { void pollHeadphones(); }

// =====================================================================
//  Board
// =====================================================================
namespace Board {

static M5Canvas* s_canvas = nullptr;
static uint16_t* s_fb = nullptr;          // the panel's frame buffer (native portrait order)
static size_t s_fbLen = 0;
static int s_nativeW = 0, s_nativeH = 0;
static int s_row0 = 0, s_rowStep = 1;   // canvas x -> frame-buffer row: row0 + x * rowStep
static int s_col0 = 0, s_colStep = 1;   // canvas y -> frame-buffer column: col0 + y * colStep
static void setupBands(int rotation);

// Write to the frame buffer in small pieces with a short gap after each, so the display
// controller's continuous reads of the same PSRAM never wait behind a long burst
// (that is what shows as "underrun" / blue flashes).
static void copyGently(uint8_t* dst, const uint8_t* src, size_t len) {
  const size_t CH = TAB5_FB_CHUNK;
  for (size_t off = 0; off < len; off += CH) {
    size_t n = min(CH, len - off);
    memcpy(dst + off, src + off, n);
    esp_cache_msync(dst + off, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    esp_rom_delay_us(TAB5_FB_GAP_US);
  }
}


const char* name() { return "Tab5"; }

// Find where three marker pixels land in a native-order 16-bit buffer (w x h). Markers
// drawn at canvas (0,0), (1,0), (0,1) always end up in one of the four corners, so only
// the corners are searched (scanning all 1.8 MB of PSRAM would make the screen flicker).
static bool findMarkers(const uint16_t* buf, int w, int h, int pos[3]) {
  static const uint16_t marks[3] = {0x1234, 0x2345, 0x3456};
  const int xs[4] = {0, w - 3, 0, w - 3}, ys[4] = {0, 0, h - 3, h - 3};
  for (int m = 0; m < 3; m++) {
    pos[m] = -1;
    for (int c = 0; c < 4 && pos[m] < 0; c++)
      for (int y = ys[c]; y < ys[c] + 3 && pos[m] < 0; y++)
        for (int x = xs[c]; x < xs[c] + 3; x++)
          if (buf[y * w + x] == marks[m]) { pos[m] = y * w + x; break; }
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
  // Backlight off until the first real frame is on screen: the DSI panel shows garbage
  // and flickers while it starts up (same fix as the AetherScan firmware).
  M5.Display.setBrightness(0);
  M5.Display.setRotation(TAB5_ROTATION);    // (M5.begin() already cleared the screen)
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
    drawMarkers(M5.Display);
    M5.Display.display();
    if (findMarkers(s_fb, s_nativeW, s_nativeH, want)) {
      for (int r = 0; r < 8 && !fast; r++) {
        s_canvas->setRotation(r);
        drawMarkers(*s_canvas);
        if (findMarkers((const uint16_t*)s_canvas->getBuffer(), s_nativeW, s_nativeH, got) &&
            got[0] == want[0] && got[1] == want[1] && got[2] == want[2]) fast = true;
        for (int m = 0; m < 3; m++) if (got[m] >= 0) ((uint16_t*)s_canvas->getBuffer())[got[m]] = 0;   // erase
      }
    }
    M5.Display.drawPixel(0, 0, TFT_BLACK);     // erase just the markers (a full clear
    M5.Display.drawPixel(1, 0, TFT_BLACK);     // is a big PSRAM burst that flickers)
    M5.Display.drawPixel(0, 1, TFT_BLACK);
  }
  if (!fast) {                            // fall back: landscape canvas, library copy (slow)
    s_fb = nullptr;
    s_canvas->deleteSprite();
    s_canvas->createSprite(w, h);
    log_w("Tab5: frame buffer mapping not found - using slow screen updates");
  }
  if (fast) {
    s_row0 = want[0] / s_nativeW;               // frame-buffer row of canvas x = 0
    s_rowStep = (want[1] - want[0]) / s_nativeW >= 0 ? 1 : -1;
    s_col0 = want[0] % s_nativeW;               // frame-buffer column of canvas y = 0
    s_colStep = (want[2] - want[0]) >= 0 ? 1 : -1;
#if TAB5_BANDED
    setupBands(s_canvas->getRotation());
#endif
  }
  s_canvas->fillScreen(TFT_BLACK);
  log_w("Tab5 display %dx%d, canvas rotation %d, %s updates", s_canvas->width(), s_canvas->height(),
        s_canvas->getRotation(), s_fb ? "fast" : "slow");
  return true;
}

int frameWidth() { return s_fb ? s_nativeW : s_canvas->width(); }
int frameHeight() { return s_fb ? s_nativeH : s_canvas->height(); }
bool frameLittleEndian() { return true; }
const void* frameData() { return s_fb ? (const void*)s_fb : s_canvas->getBuffer(); }

// ---- banded drawing ----
static M5Canvas* s_band = nullptr;
static int s_bandRows = 0, s_nBands = 0;
static uint32_t* s_bandHash = nullptr;

// Two band buffers in internal RAM: while the DMA engine copies one to the screen,
// the next band is drawn into the other.
static uint8_t* s_bandBuf[2] = {nullptr, nullptr};
static SemaphoreHandle_t s_bandDone[2] = {nullptr, nullptr};
static bool s_bandBusy[2] = {false, false};
static async_memcpy_handle_t s_dma = nullptr;
static int s_bandRot = 0;

static bool IRAM_ATTR dmaDone(async_memcpy_handle_t, async_memcpy_event_t*, void* arg) {
  BaseType_t woke = pdFALSE;
  xSemaphoreGiveFromISR((SemaphoreHandle_t)arg, &woke);
  return woke == pdTRUE;
}

static void setupBands(int rotation) {
  if (!s_fb) return;
  for (int rows : {40, 32, 20}) {             // bands that fit twice in internal RAM, leaving room for Wi-Fi
    if (s_nativeH % rows) continue;
    size_t len = (size_t)s_nativeW * rows * 2;
    s_bandBuf[0] = (uint8_t*)heap_caps_aligned_alloc(64, len, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    s_bandBuf[1] = (uint8_t*)heap_caps_aligned_alloc(64, len, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
    if (s_bandBuf[0] && s_bandBuf[1]) { s_bandRows = rows; break; }
    heap_caps_free(s_bandBuf[0]); heap_caps_free(s_bandBuf[1]);
    s_bandBuf[0] = s_bandBuf[1] = nullptr;
  }
  if (!s_bandRows) { log_w("Tab5: no internal RAM for banded drawing"); return; }
  s_band = new M5Canvas(&M5.Display);
  s_bandRot = rotation;
  s_nBands = s_nativeH / s_bandRows;
  s_bandHash = (uint32_t*)heap_caps_calloc(s_nBands, sizeof(uint32_t), MALLOC_CAP_INTERNAL);
  for (int k = 0; k < s_nBands; k++) s_bandHash[k] = ~0u;
  for (int i = 0; i < 2; i++) { s_bandDone[i] = xSemaphoreCreateBinary(); }
  async_memcpy_config_t cfg = ASYNC_MEMCPY_DEFAULT_CONFIG();
  cfg.backlog = 4;
  cfg.dma_burst_size = TAB5_DMA_BURST;
  if (esp_async_memcpy_install_gdma_axi(&cfg, &s_dma) != ESP_OK) { s_dma = nullptr; log_w("Tab5: no DMA copy, using CPU"); }
  log_w("Tab5: banded drawing, %d bands of %d rows, %s copies", s_nBands, s_bandRows, s_dma ? "DMA" : "CPU");
}

static void waitBand(int i) {
  if (s_bandBusy[i]) { xSemaphoreTake(s_bandDone[i], pdMS_TO_TICKS(200)); s_bandBusy[i] = false; }
}

int bands() { return s_nBands; }

LGFX_Sprite& band(int k, int& xOffset) {
  int i = k & 1;
  waitBand(i);                                // its previous copy must be finished
  s_band->setBuffer(s_bandBuf[i], s_nativeW, s_bandRows, s_canvas->getColorDepth());
  s_band->setRotation(s_bandRot);
  int r0 = k * s_bandRows;                    // first frame-buffer row of this band
  xOffset = s_rowStep > 0 ? r0 - s_row0 : s_row0 - r0 - s_bandRows + 1;
  s_band->fillScreen(TFT_BLACK);
  return *s_band;
}

void bandsDone() { waitBand(0); waitBand(1); }

static uint32_t hashBuf(const uint32_t* p, size_t words) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < words; i++) h = (h ^ p[i]) * 16777619u;
  return h;
}

void presentBand(int k) {
  int i = k & 1;
  size_t len = (size_t)s_nativeW * s_bandRows * 2;
  auto* src = (const uint32_t*)s_bandBuf[i];
  uint32_t h = hashBuf(src, len / 4);
  if (h == s_bandHash[k]) return;             // unchanged: leave the screen alone
  s_bandHash[k] = h;
  auto* dst = (uint8_t*)s_fb + (size_t)k * len;
  if (s_dma) {
    esp_cache_msync(s_bandBuf[i], len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    if (esp_async_memcpy(s_dma, dst, s_bandBuf[i], len, dmaDone, s_bandDone[i]) == ESP_OK) { s_bandBusy[i] = true; return; }
  }
  copyGently(dst, (const uint8_t*)src, len);
}


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
  if (s_bandHash) for (int k = 0; k < s_nBands; k++) s_bandHash[k] = ~0u;   // screen no longer matches bands
  if (s_fb) {
    const int rowBytes = s_nativeW * 2, words = rowBytes / 4;
    if (!s_rowHash) {
      s_rowHash = (uint32_t*)heap_caps_calloc(s_nativeH, sizeof(uint32_t), MALLOC_CAP_INTERNAL);
      if (!s_rowHash) {                   // no memory for fingerprints: copy everything
        copyGently((uint8_t*)s_fb, (const uint8_t*)s_canvas->getBuffer(), s_fbLen);
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
      copyGently(dst + off, src + off, len);
    }
  } else {
    s_canvas->pushSprite(0, 0);
  }
}
// Copy one rectangle of the canvas (canvas pixels) to the screen.
void presentRect(int x, int y, int w, int h) {
  if (!s_fb) { present(); return; }
  int rA = s_row0 + x * s_rowStep, rB = s_row0 + (x + w - 1) * s_rowStep;
  int cA = s_col0 + y * s_colStep, cB = s_col0 + (y + h - 1) * s_colStep;
  int r0 = max(0, min(rA, rB)), r1 = min(s_nativeH - 1, max(rA, rB));
  int c0 = max(0, min(cA, cB)), c1 = min(s_nativeW - 1, max(cA, cB));
  if (r1 < r0 || c1 < c0) return;
  if (s_bandHash) for (int k = 0; k < s_nBands; k++) s_bandHash[k] = ~0u;
  auto* src = (const uint16_t*)s_canvas->getBuffer();
  size_t seg = (size_t)(c1 - c0 + 1) * 2;
  for (int r = r0; r <= r1; r++) {
    size_t off = (size_t)r * s_nativeW + c0;
    memcpy(s_fb + off, src + off, seg);
  }
  uint8_t* a0 = (uint8_t*)(s_fb + (size_t)r0 * s_nativeW + c0);
  uint8_t* a1 = (uint8_t*)(s_fb + (size_t)r1 * s_nativeW + c1 + 1);
  esp_cache_msync(a0, a1 - a0, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

void setBrightness(int pct) { M5.Display.setBrightness(constrain(pct, 0, 100) * 255 / 100); }

// ---- automatic 180-degree flip from the IMU ----
// Upright in TAB5_ROTATION the accelerometer reads about +1 g on X; held the other way up
// it reads -1 g. Portrait or lying flat keeps the current orientation.
static bool s_flipped = false;
static volatile bool s_rotated = false;

static void applyFlip(bool flipped) {
  s_flipped = flipped;
  int disp = flipped ? (TAB5_ROTATION + 2) % 4 : TAB5_ROTATION;
  M5.Display.setRotation(disp);                      // touch follows the display rotation
  int r = s_canvas->getRotation();
  int r2 = (((r & 3) + 2) & 3) | (r & 4);             // same mapping turned 180 degrees
  s_canvas->setRotation(r2);
  s_row0 = s_nativeH - 1 - s_row0;                   // the mapping turns over too
  s_rowStep = -s_rowStep;
  s_col0 = s_nativeW - 1 - s_col0;
  s_colStep = -s_colStep;
  if (s_band) {
    bandsDone();
    s_bandRot = r2;
    for (int k = 0; k < s_nBands; k++) s_bandHash[k] = ~0u;   // redraw everything
  }
  s_rotated = true;
}

static void autoRotate() {
#if TAB5_AUTO_ROTATE
  static uint32_t last = 0, since = 0;
  static int pending = 0;
  if (millis() - last < 200) return;
  last = millis();
  float ax, ay, az;
  if (!M5.Imu.getAccel(&ax, &ay, &az)) return;
  int want = ax > 0.6f ? 1 : ax < -0.6f ? -1 : 0;     // 1 = normal, -1 = upside down
  bool flip = want < 0;
  if (want == 0 || flip == s_flipped) { pending = 0; return; }
  if (pending != want) { pending = want; since = millis(); return; }
  if (millis() - since >= 800) { pending = 0; applyFlip(flip); }
#endif
}

bool orientationChanged() {
  bool r = s_rotated;
  s_rotated = false;
  return r;
}

bool touch(int& x, int& y) {
  M5.update();
  autoRotate();
  AudioOut::pollHeadphones();
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

// Headphone-detect line on the Tab5's IO expander (PI4IOE5V6408 at 0x43, input bit 7).
// For now this only reports changes on the Serial Monitor.
void pollHeadphones() {
  static uint32_t last = 0;
  static int prev = -1;
  if (millis() - last < 500) return;
  last = millis();
  int v = (M5.In_I2C.readRegister8(0x43, 0x0F, 400000) >> 7) & 1;
  if (v != prev) { Serial.printf("Headphone detect line: %d\n", v); prev = v; }
}

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
  // M5Unified gives the Tab5 speaker a digital gain of only 4, which is quiet;
  // TAB5_SPK_GAIN raises it (the 3.5 mm jack gets the same signal).
  M5.Speaker.end();
  auto sc = M5.Speaker.config();
  sc.magnification = TAB5_SPK_GAIN;
  M5.Speaker.config(sc);
  M5.Speaker.begin();
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


