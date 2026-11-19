#include "calls.h"
#include "config.h"
#include "settings.h"
#include "audio_out.h"
#include <FS.h>
#if defined(BOARD_TAB5)
#include <SD.h>                  // Tab5: SPI mode (SDMMC would clash with the Wi-Fi chip's SDIO link)
#include <SPI.h>
#define SDCARD SD
#else
#include <SD_MMC.h>
#define SDCARD SD_MMC
#endif
#include <esp_heap_caps.h>
#include <time.h>
#include <atomic>
#include <math.h>

namespace Calls {

// ---------------- live audio ring (PSRAM) ----------------
static const uint32_t RING = 1 << 17;          // 131072 samples, ~16 s at 8 kHz
static int16_t* s_ring = nullptr;
static std::atomic<uint32_t> s_head{0}, s_tail{0};
static std::atomic<uint32_t> s_rate{8000};

void feed(const int16_t* pcm, size_t n, uint32_t rate) {
  if (!s_ring) return;
  s_rate.store(rate);
  uint32_t h = s_head.load(std::memory_order_relaxed);
  uint32_t t = s_tail.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; i++) {
    if (h - t >= RING - 1) break;              // task fell behind: drop
    s_ring[h & (RING - 1)] = pcm[i];
    h++;
  }
  s_head.store(h, std::memory_order_release);
}

// ---------------- shared state ----------------
static SemaphoreHandle_t s_mx;
struct Lock {
  Lock() { xSemaphoreTake(s_mx, portMAX_DELAY); }
  ~Lock() { xSemaphoreGive(s_mx); }
};

struct Ctx { bool online = false; String sys, dept, chan, tgid, freq; };
static Ctx s_ctx;

void setContext(bool online, const String& system, const String& dept, const String& channel,
                const String& tgid, const String& freq) {
  if (!s_mx) return;
  Lock l;
  s_ctx.online = online;
  s_ctx.sys = system;
  s_ctx.dept = dept;
  s_ctx.chan = channel;
  s_ctx.tgid = tgid;
  s_ctx.freq = freq;
}

static Ctx context() {
  Lock l;
  return s_ctx;
}

static Call s_recent[RECENT_MAX];
static int s_recentN = 0;
static std::atomic<uint32_t> s_recentVer{0};

static void addRecent(const Call& c) {
  {
    Lock l;
    for (int i = min(s_recentN, RECENT_MAX - 1); i > 0; i--) s_recent[i] = s_recent[i - 1];
    s_recent[0] = c;
    if (s_recentN < RECENT_MAX) s_recentN++;
  }
  s_recentVer++;
}

int recentCount() { Lock l; return s_recentN; }
uint32_t recentVersion() { return s_recentVer.load(); }
bool recent(int i, Call& out) {
  Lock l;
  if (i < 0 || i >= s_recentN) return false;
  out = s_recent[i];
  return true;
}

static std::atomic<bool> s_sd{false}, s_recording{false}, s_inCall{false};
static std::atomic<bool> s_alert{false}, s_replaying{false};
static std::atomic<uint32_t> s_freeMB{0};
bool sdOk() { return s_sd.load(); }
bool recordingNow() { return s_recording.load(); }
bool inCall() { return s_inCall.load(); }
bool alertActive() { return s_alert.load(); }
void ackAlert() { s_alert = false; }
bool replaying() { return s_replaying.load(); }

String sdInfo() {
  if (!s_sd) return "no SD card";
  char b[40];
  snprintf(b, sizeof(b), "SD card: %.1f GB free", s_freeMB.load() / 1024.0f);
  return b;
}

// ---------------- helpers ----------------
static bool clockSet(time_t t) { return t > 1700000000; }

static String dateStr(time_t t) {
  struct tm tm;
  localtime_r(&t, &tm);
  char b[16];
  strftime(b, sizeof(b), "%Y-%m-%d", &tm);
  return b;
}

static String safeName(const String& s) {
  String o;
  for (size_t i = 0; i < s.length() && o.length() < 32; i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '_') o += c;
    else if (o.length() && o[o.length() - 1] != '_') o += '_';
  }
  while (o.endsWith("_")) o.remove(o.length() - 1);
  return o.length() ? o : String("call");
}

static String csvField(String s) {
  s.replace("\"", "'");
  s.trim();
  return "\"" + s + "\"";
}

static void ensureDir(const String& path) {
  // create every directory in "path" (which is a directory)
  for (int i = 1; i <= (int)path.length(); i++) {
    if (i == (int)path.length() || path[i] == '/') {
      String d = path.substring(0, i);
      if (!SDCARD.exists(d)) SDCARD.mkdir(d);
    }
  }
}

static void updateFree() {
  if (!s_sd) return;
  uint64_t total = SDCARD.totalBytes(), used = SDCARD.usedBytes();
  s_freeMB = (uint32_t)((total - used) / (1024 * 1024));
}

// ---------------- WAV writer ----------------
static File s_wav;
static String s_wavPath;
static uint32_t s_wavBytes = 0, s_wavRate = 8000;
static int16_t* s_wbuf = nullptr;              // write buffer (PSRAM)
static const size_t WBUF = 8192;               // samples
static size_t s_wn = 0;

static void put32(uint8_t* p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(uint8_t* p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

static void wavHeader(uint8_t* h, uint32_t dataBytes, uint32_t rate) {
  memcpy(h, "RIFF", 4); put32(h + 4, 36 + dataBytes);
  memcpy(h + 8, "WAVEfmt ", 8); put32(h + 16, 16);
  put16(h + 20, 1); put16(h + 22, 1);           // PCM, mono
  put32(h + 24, rate); put32(h + 28, rate * 2);
  put16(h + 32, 2); put16(h + 34, 16);
  memcpy(h + 36, "data", 4); put32(h + 40, dataBytes);
}

static bool wavOpen(const String& dir, const String& name, uint32_t rate) {
  ensureDir(dir);
  s_wavPath = dir + "/" + name;
  s_wav = SDCARD.open(s_wavPath, FILE_WRITE);
  if (!s_wav) { log_w("can't create %s", s_wavPath.c_str()); s_wavPath = ""; return false; }
  uint8_t h[44];
  wavHeader(h, 0, rate);
  s_wav.write(h, 44);
  s_wavBytes = 0;
  s_wavRate = rate;
  s_wn = 0;
  s_recording = true;
  return true;
}

static void wavFlush() {
  if (s_wav && s_wn) {
    s_wav.write((const uint8_t*)s_wbuf, s_wn * 2);
    s_wavBytes += s_wn * 2;
  }
  s_wn = 0;
}

static void wavWrite(const int16_t* p, size_t n) {
  if (!s_wav) return;
  while (n) {
    size_t k = min(n, WBUF - s_wn);
    memcpy(s_wbuf + s_wn, p, k * 2);
    s_wn += k; p += k; n -= k;
    if (s_wn == WBUF) wavFlush();
  }
}

// Returns the file path if kept.
static String wavClose(bool keep) {
  String path;
  if (s_wav) {
    wavFlush();
    if (keep) {
      uint8_t h[44];
      wavHeader(h, s_wavBytes, s_wavRate);
      s_wav.seek(0);
      s_wav.write(h, 44);
      path = s_wavPath;
    }
    s_wav.close();
    if (!keep) SDCARD.remove(s_wavPath);
  }
  s_wavPath = "";
  s_recording = false;
  return path;
}

// ---------------- call log (CSV) ----------------
static const char* CSV_HEADER = "date,time,duration_s,system,department,channel,tgid,frequency,recording\n";

static void logCall(const Call& c) {
  if (!s_sd) return;
  String date = clockSet(c.start) ? dateStr(c.start) : String("undated");
  ensureDir("/logs");
  String path = "/logs/" + date + ".csv";
  bool fresh = !SDCARD.exists(path);
  File f = SDCARD.open(path, FILE_APPEND);
  if (!f) return;
  if (fresh) f.print(CSV_HEADER);
  char t[12] = "";
  if (clockSet(c.start)) { struct tm tm; localtime_r(&c.start, &tm); strftime(t, sizeof(t), "%H:%M:%S", &tm); }
  String line = date + "," + t + "," + String(c.durMs / 1000.0f, 1) + "," + csvField(c.system) + "," +
                csvField(c.dept) + "," + csvField(c.channel) + "," + csvField(c.tgid) + "," +
                csvField(c.freq) + "," + csvField(c.wav) + "\n";
  f.print(line);
  f.close();
}

// Split one CSV line (fields may be in double quotes).
static int splitCsv(const String& line, String* out, int max) {
  int n = 0;
  String cur;
  bool q = false;
  for (size_t i = 0; i < line.length(); i++) {
    char c = line[i];
    if (c == '"') q = !q;
    else if (c == ',' && !q) { if (n < max) out[n++] = cur; cur = ""; }
    else if (c != '\r' && c != '\n') cur += c;
  }
  if (n < max) out[n++] = cur;
  return n;
}

// Fill the RECENT list from today's log (after a restart).
static void loadTodaysLog() {
  time_t now = time(nullptr);
  String path = "/logs/" + dateStr(now) + ".csv";
  if (!SDCARD.exists(path)) return;
  File f = SDCARD.open(path, FILE_READ);
  if (!f) return;
  // keep the last RECENT_MAX lines
  String lines[RECENT_MAX];
  int n = 0;
  while (f.available()) {
    String l = f.readStringUntil('\n');
    if (l.startsWith("date,") || l.length() < 10) continue;
    lines[n % RECENT_MAX] = l;
    n++;
  }
  f.close();
  int first = n > RECENT_MAX ? n - RECENT_MAX : 0;
  for (int k = first; k < n; k++) {
    String fl[9];
    if (splitCsv(lines[k % RECENT_MAX], fl, 9) < 9) continue;
    struct tm tm = {};
    if (sscanf((fl[0] + " " + fl[1]).c_str(), "%d-%d-%d %d:%d:%d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6) continue;
    tm.tm_year -= 1900; tm.tm_mon -= 1; tm.tm_isdst = -1;
    Call c;
    c.start = mktime(&tm);
    c.startMs = 0;
    c.durMs = (uint32_t)(fl[2].toFloat() * 1000);
    c.system = fl[3]; c.dept = fl[4]; c.channel = fl[5]; c.tgid = fl[6]; c.freq = fl[7]; c.wav = fl[8];
    addRecent(c);
  }
  log_w("loaded %d calls from %s", min(n, RECENT_MAX), path.c_str());
}

// ---------------- replay ----------------
static String s_playPath;
static std::atomic<bool> s_playReq{false}, s_stopReq{false};
static File s_play;
static uint32_t s_playRate = 8000;

bool play(int i) {
  Call c;
  if (!s_sd || !recent(i, c) || c.wav.length() == 0) return false;
  { Lock l; s_playPath = c.wav; }
  s_stopReq = false;
  s_playReq = true;
  return true;
}
void stopPlay() { s_stopReq = true; }

static void endPlay() {
  if (s_play) s_play.close();
  AudioOut::flush();
  AudioOut::setStreamRate(s_rate.load());
  s_replaying = false;
}

static void servicePlay() {
  if (s_playReq.exchange(false)) {
    if (s_play) s_play.close();
    String p;
    { Lock l; p = s_playPath; }
    s_play = SDCARD.open(p, FILE_READ);
    uint8_t h[44];
    if (!s_play || s_play.read(h, 44) != 44 || memcmp(h, "RIFF", 4) != 0) { endPlay(); return; }
    s_playRate = h[24] | (h[25] << 8) | (h[26] << 16) | ((uint32_t)h[27] << 24);
    s_replaying = true;
    AudioOut::flush();
    AudioOut::setStreamRate(s_playRate);
  }
  if (!s_replaying) return;
  if (s_stopReq.exchange(false)) { endPlay(); return; }
  static int16_t buf[512];
  while (AudioOut::queued() < s_playRate / 2) {
    int got = s_play.read((uint8_t*)buf, sizeof(buf));
    if (got <= 0) {
      if (AudioOut::queued() == 0) endPlay();
      return;
    }
    AudioOut::push(buf, got / 2);
  }
}

// ---------------- detector task ----------------
static const int MAX_BLOCK = 2400;             // 50 ms at 48 kHz
static const int PREROLL = 4;                  // blocks kept from just before a call starts
static int16_t* s_blk = nullptr;
static int16_t* s_hist = nullptr;              // PREROLL blocks
static int s_histN[PREROLL];
static int s_histPos = 0;

static bool s_call = false;
static Call s_cur;
static String s_curKey;
static uint32_t s_lastVoice = 0;
static std::atomic<bool> s_closeReq{false}, s_closeDone{false};

static uint32_t s_toneMs = 0;
static int s_toneGap = 0;
static bool s_toneLatched = false;

static void startCall(const Ctx& c, const int16_t* blk, int n, uint32_t rate) {
  s_call = true;
  s_inCall = true;
  s_cur = Call();
  s_cur.start = time(nullptr);
  if (!clockSet(s_cur.start)) s_cur.start = 0;
  s_cur.startMs = millis();
  s_cur.system = c.sys; s_cur.dept = c.dept; s_cur.channel = c.chan; s_cur.tgid = c.tgid; s_cur.freq = c.freq;
  s_curKey = c.chan + "|" + c.tgid;
  s_lastVoice = millis();

  if (s_sd && Settings::get().record) {
    String dir, name;
    if (s_cur.start) {
      struct tm tm;
      localtime_r(&s_cur.start, &tm);
      char hms[8];
      strftime(hms, sizeof(hms), "%H%M%S", &tm);
      dir = "/rec/" + dateStr(s_cur.start);
      name = String(hms) + "_" + safeName(c.chan.length() ? c.chan : c.tgid) + ".wav";
    } else {
      dir = "/rec/undated";
      name = String(millis()) + "_" + safeName(c.chan) + ".wav";
    }
    if (wavOpen(dir, name, rate)) {
      for (int k = 1; k <= PREROLL; k++) {      // oldest first
        int idx = (s_histPos + k) % PREROLL;
        if (s_histN[idx]) wavWrite(s_hist + idx * MAX_BLOCK, s_histN[idx]);
      }
      wavWrite(blk, n);
    }
  }
}

static void endCall() {
  if (!s_call) return;
  s_call = false;
  s_inCall = false;
  uint32_t dur = s_lastVoice - s_cur.startMs + 50;
  if (dur < REC_MIN_MS) { wavClose(false); return; }
  s_cur.durMs = dur;
  s_cur.wav = wavClose(true);
  addRecent(s_cur);
  logCall(s_cur);
  Serial.printf("call: %s / %s  %.1fs%s\n", s_cur.channel.c_str(), s_cur.tgid.c_str(), dur / 1000.0f,
                s_cur.wav.length() ? (" -> " + s_cur.wav).c_str() : "");
}

static void processBlock(const int16_t* b, int n, uint32_t rate) {
  // level and 1050 Hz (Goertzel)
  double energy = 0;
  float coeff = 2.0f * cosf(2.0f * PI * 1050.0f / rate);
  float s1 = 0, s2 = 0;
  for (int i = 0; i < n; i++) {
    float x = b[i];
    energy += x * x;
    float s0 = x + coeff * s1 - s2;
    s2 = s1; s1 = s0;
  }
  float rms = sqrtf(energy / n);
  float tone = s1 * s1 + s2 * s2 - coeff * s1 * s2;
  float ratio = energy > 0 ? tone / (energy * n / 2.0) : 0;

  // weather alert tone
  if (rms > REC_SILENCE_RMS && ratio > 0.5f) {
    s_toneMs += 50; s_toneGap = 0;
    if (s_toneMs >= WX_ALERT_TONE_MS && !s_toneLatched) {
      s_toneLatched = true;
      s_alert = true;
      Serial.println("WEATHER ALERT tone detected");
    }
  } else if (++s_toneGap > 2) {
    s_toneMs = 0;
    s_toneLatched = false;
  }

  // calls
  Ctx c = context();
  bool voice = rms > REC_SILENCE_RMS && c.online;
  uint32_t now = millis();
  if (s_call) {
    if (voice && (c.chan + "|" + c.tgid) != s_curKey) endCall();   // new channel: new call
    else {
      wavWrite(b, n);
      if (voice) s_lastVoice = now;
      if (now - s_lastVoice > REC_HANG_MS) endCall();
    }
  }
  if (!s_call && voice) startCall(c, b, n, rate);

  // keep a little history for the start of the next call
  s_histPos = (s_histPos + 1) % PREROLL;
  memcpy(s_hist + s_histPos * MAX_BLOCK, b, n * 2);
  s_histN[s_histPos] = n;
}

static bool mountSd() {
#if defined(BOARD_TAB5)
  SPI.begin(TAB5_SD_SCK, TAB5_SD_MISO, TAB5_SD_MOSI, TAB5_SD_CS);
  bool ok = SD.begin(TAB5_SD_CS, SPI, 25000000);
#else
  SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0, PIN_SD_D1, PIN_SD_D2, PIN_SD_D3);
  bool ok = SD_MMC.begin("/sdcard", false);                 // 4-bit
  if (!ok) {
    SD_MMC.end();
    SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0);
    ok = SD_MMC.begin("/sdcard", true);                     // 1-bit
  }
#endif
  if (!ok || SDCARD.cardType() == CARD_NONE) { Serial.println("SD card: not found"); return false; }
  Serial.printf("SD card: %llu MB\n", SDCARD.cardSize() / (1024 * 1024));
  return true;
}

static void task(void*) {
  s_sd = mountSd();
  updateFree();
  if (s_sd) Serial.println(sdInfo());
  bool logLoaded = false;
  uint32_t lastFree = millis();

  for (;;) {
    servicePlay();
    if (s_closeReq.load()) {
      endCall();
      if (s_replaying) endPlay();
      s_closeDone = true;
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    uint32_t rate = s_rate.load();
    int n = min<int>(rate / 20, MAX_BLOCK);
    uint32_t t = s_tail.load(std::memory_order_relaxed);
    if (s_head.load(std::memory_order_acquire) - t >= (uint32_t)n) {
      for (int i = 0; i < n; i++) s_blk[i] = s_ring[(t + i) & (RING - 1)];
      s_tail.store(t + n, std::memory_order_release);
      processBlock(s_blk, n, rate);
      continue;
    }

    // no audio waiting
    if (s_call && millis() - s_lastVoice > REC_HANG_MS) endCall();
    if (!s_call) {
      s_toneMs = 0;
      if (s_sd && !logLoaded && clockSet(time(nullptr))) { logLoaded = true; loadTodaysLog(); }
      if (s_sd && millis() - lastFree > 600000) { lastFree = millis(); updateFree(); }
    }
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

void begin() {
  s_mx = xSemaphoreCreateMutex();
  s_ring = (int16_t*)heap_caps_malloc(RING * 2, MALLOC_CAP_SPIRAM);
  s_wbuf = (int16_t*)heap_caps_malloc(WBUF * 2, MALLOC_CAP_SPIRAM);
  s_hist = (int16_t*)heap_caps_calloc(PREROLL * MAX_BLOCK, 2, MALLOC_CAP_SPIRAM);
  s_blk = (int16_t*)heap_caps_malloc(MAX_BLOCK * 2, MALLOC_CAP_SPIRAM);
  if (!s_ring || !s_wbuf || !s_hist || !s_blk) { log_e("calls: out of memory"); s_ring = nullptr; return; }
  xTaskCreatePinnedToCore(task, "calls", 8192, nullptr, 1, nullptr, 1);
}

void closeAll() {
  if (!s_ring) return;
  s_closeDone = false;
  s_closeReq = true;
  uint32_t start = millis();
  while (!s_closeDone && millis() - start < 1500) delay(10);
}

}  // namespace Calls
