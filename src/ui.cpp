#include "ui.h"
#include "config.h"
#include "board.h"
#include "sds200.h"
#include "rtsp_audio.h"
#include "audio_out.h"
#include "battery.h"
#include "power.h"
#include "settings.h"
#include "calls.h"
#include "ota.h"
#include <WiFi.h>
#include <time.h>
#include <vector>
#include <algorithm>

namespace Ui {

static const int W = 480, H = 320;

// Boot logo and animation, built into the firmware (see tools/make_boot_assets.sh).
extern const uint8_t kLogoStart[] asm("_binary_data_boot_logo_jpg_start");
extern const uint8_t kLogoEnd[]   asm("_binary_data_boot_logo_jpg_end");
extern const uint8_t kAnimStart[] asm("_binary_data_boot_anim_bin_start");
// The UI is laid out on a logical 480x320 screen. Pen draws it onto the board's
// canvas, scaling positions and swapping in larger fonts on bigger screens
// (scale 1 on the FNK0104N; 2.67 x 2.25 on the Tab5's 1280x720).
struct Pen {
  LGFX_Sprite* c = nullptr;
  float kx = 1, ky = 1;
  int X(int x) const { return (int)lroundf(x * kx); }
  int Y(int y) const { return (int)lroundf(y * ky); }
  int R(int r) const { return (int)lroundf(r * (kx < ky ? kx : ky)); }
  bool scaled() const { return kx != 1 || ky != 1; }

  const lgfx::IFont* mapFont(const lgfx::IFont* f) const {
    if (!scaled()) return f;
    // Pick a smooth font about ky times the original size.
    if (f == &fonts::Font0) return &fonts::DejaVu24;
    if (f == &fonts::Font2) return &fonts::DejaVu40;
    if (f == &fonts::Font4) return &fonts::DejaVu56;
    if (f == &fonts::FreeSans12pt7b || f == &fonts::FreeSansBold12pt7b) return &fonts::DejaVu56;
    if (f == &fonts::FreeSansBold18pt7b) return &fonts::DejaVu72;
    return f;
  }

  void fillScreen(uint16_t col) { c->fillScreen(col); }
  void fillRect(int x, int y, int w, int h, uint16_t col) { c->fillRect(X(x), Y(y), X(x + w) - X(x), Y(y + h) - Y(y), col); }
  void drawRect(int x, int y, int w, int h, uint16_t col) { c->drawRect(X(x), Y(y), X(x + w) - X(x), Y(y + h) - Y(y), col); }
  void fillRoundRect(int x, int y, int w, int h, int r, uint16_t col) { c->fillRoundRect(X(x), Y(y), X(x + w) - X(x), Y(y + h) - Y(y), R(r), col); }
  void drawRoundRect(int x, int y, int w, int h, int r, uint16_t col) { c->drawRoundRect(X(x), Y(y), X(x + w) - X(x), Y(y + h) - Y(y), R(r), col); }
  void drawFastHLine(int x, int y, int w, uint16_t col) { c->fillRect(X(x), Y(y), X(x + w) - X(x), Y(y + 1) - Y(y), col); }
  void fillCircle(int x, int y, int r, uint16_t col) { c->fillCircle(X(x), Y(y), R(r), col); }
  void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t col) { c->fillTriangle(X(x0), Y(y0), X(x1), Y(y1), X(x2), Y(y2), col); }
  void setFont(const lgfx::IFont* f) { c->setFont(mapFont(f)); }
  void setTextColor(uint16_t col) { c->setTextColor(col); }
  void setTextDatum(lgfx::textdatum_t d) { c->setTextDatum(d); }
  void drawString(const char* t, int x, int y) { c->drawString(t, X(x), Y(y)); }
  int textWidth(const char* t) { return (int)ceilf(c->textWidth(t) / kx); }
  bool drawJpg(const uint8_t* d, uint32_t len, int x, int y) {
    return c->drawJpg(d, len, X(x), Y(y), 0, 0, 0, 0, kx, ky);
  }
};
static Pen s_canvas;

// ---------- colours ----------
static const uint16_t C_BG = 0x0000;
static const uint16_t C_PANEL = 0x18E3;
static const uint16_t C_BTN = 0x2945;
static const uint16_t C_BTN_DOWN = 0x5AEB;
static const uint16_t C_TEXT = 0xFFFF;
static const uint16_t C_DIM = 0x8410;
static const uint16_t C_SYS = 0x07FF;     // cyan
static const uint16_t C_DEPT = 0xFFE0;    // yellow
static const uint16_t C_OK = 0x07E0;      // green
static const uint16_t C_BAD = 0xF800;     // red
static const uint16_t C_DARKRED = 0x7800;
static const uint16_t C_HOLD = 0xFD20;    // orange

// ---------- pages and buttons ----------
enum Page { PAGE_MAIN, PAGE_KEYS, PAGE_POWER, PAGE_SETTINGS, PAGE_RECENT, PAGE_FAVS, PAGE_SETUP, PAGE_WIFI, PAGE_KB };
static Page s_page = PAGE_MAIN;
static Page s_pageBeforePower = PAGE_MAIN;

enum Action {
  A_NONE, A_KEY, A_SPK_DN, A_SPK_UP, A_SPK_MUTE, A_GOTO,
  A_AUDIO_RECONNECT, A_REBOOT, A_WX, A_SCAN, A_POWER_OFF, A_RESTART, A_POWER_CANCEL, A_POWER_MENU, A_LAUNCHER,
  A_REC, A_KEYLIGHT, A_RECENT_ROW, A_LIST_UP, A_LIST_DN, A_STOP_PLAY, A_FAV, A_ADJ,
  A_SETUP_FIELD, A_WIFI_PICK, A_WIFI_OTHER, A_WIFI_RESCAN, A_SAVE_CONNECT, A_FORGET_WIFI,
  A_KB_CHAR, A_KB_SHIFT, A_KB_MODE, A_KB_BKSP, A_KB_SHOW, A_KB_OK, A_KB_CANCEL
};

struct Button {
  int x, y, w, h;
  String label;
  Action action;
  int arg;
  bool hl;
  uint16_t bg;     // C_BTN = normal key; C_BAD / C_DARKRED = red-lit key; C_GREENKEY = green-lit key
  int8_t glow;     // >= 0: force this key-light colour (swatches)
};

// ---------- key backlight colours ----------
struct KeyLight { const char* name; uint8_t r, g, b; };
static const KeyLight kKeyLights[] = {
  {"Amber", 255, 168, 0},   {"Orange", 255, 110, 20}, {"Red", 255, 50, 45},    {"Yellow", 255, 235, 40},
  {"Green", 60, 255, 100},  {"Ice", 120, 230, 255},   {"Blue", 70, 140, 255},  {"Purple", 185, 105, 255},
  {"Pink", 255, 105, 200},  {"White", 240, 244, 255},
};
static const int NUM_KEYLIGHTS = sizeof(kKeyLights) / sizeof(kKeyLights[0]);
static const uint16_t C_GREENKEY = 0x0320;
static const int KL_RED = 2, KL_GREEN = 4;

// A key light at a few brightness levels (computed once per colour).
struct Glow { uint16_t full, bright, mid, dim, faint, body, bodyTop; };
static Glow makeGlow(int i) {
  const KeyLight& k = kKeyLights[constrain(i, 0, NUM_KEYLIGHTS - 1)];
  auto c = [&](int pct) { return lgfx::color565(k.r * pct / 100, k.g * pct / 100, k.b * pct / 100); };
  // pressed body: charcoal mixed with the light colour
  auto mix = [&](int base, int pct) { return lgfx::color565(base + k.r * pct / 100, base + k.g * pct / 100, base + k.b * pct / 100); };
  return {c(100), c(92), c(55), c(38), c(22), mix(20, 22), mix(28, 30)};
}
static Glow s_glow[NUM_KEYLIGHTS];
static void initGlows() { for (int i = 0; i < NUM_KEYLIGHTS; i++) s_glow[i] = makeGlow(i); }

static std::vector<Button> s_btns;     // buttons of the page on screen (rebuilt every redraw)
static int s_pressed = -1;
static uint32_t s_pressStart = 0;
static bool s_longSent = false;
static bool s_dirty = true;
static uint32_t s_lastVersion = 0;
static uint32_t s_lastDraw = 0;
static String s_toast;
static uint32_t s_toastUntil = 0;

static void toast(const String& t) { s_toast = t; s_toastUntil = millis() + 1500; s_dirty = true; }

static void add(int x, int y, int w, int h, const String& label, Action a, int arg = 0, bool hl = false, uint16_t bg = C_BTN, int8_t glow = -1) {
  s_btns.push_back({x, y, w, h, label, a, arg, hl, bg, glow});
}

// Reboot needs a second tap within this window.
static const uint32_t REBOOT_CONFIRM_MS = 4000;
static uint32_t s_rebootArmedUntil = 0;
static bool rebootArmed() { return s_rebootArmedUntil && millis() < s_rebootArmedUntil; }

// ---------- dimming ----------
static uint32_t s_lastActivity = 0;
static bool s_dimmed = false;
static int s_appliedBrightness = -1;
static bool s_swallowTouch = false;     // first touch on a dim screen only wakes it

static void activity() { s_lastActivity = millis(); }

static void applyBrightness() {
  const auto& cfg = Settings::get();
  int want = s_dimmed ? cfg.dimLevel : cfg.brightness;
  if (want != s_appliedBrightness) { s_appliedBrightness = want; Board::setBrightness(want); }
}

// ---------- weather alert ----------
static bool s_gsiAlertAck = false;
static uint32_t s_lastBeep = 0;
static bool gsiAlert() { return Sds200::status().wxMode.indexOf("Alert") >= 0; }
static bool alertShowing() { return Calls::alertActive() || (gsiAlert() && !s_gsiAlertAck); }

// ---------- list / setup state ----------
static int s_recentTop = 0;
static int s_playingRow = -1;

struct Net { String ssid; int rssi; bool open; };
static std::vector<Net> s_nets;
static int s_netTop = 0;

static bool s_firstRun = false;
static String s_eSsid, s_ePass, s_eIp, s_eOta;   // values being edited on the SETUP page
static String s_setupMsg;

enum KbMode { KB_LOWER, KB_UPPER, KB_NUM, KB_SYM, KB_IP };
static int s_kbField = 0;               // 0 ssid, 1 wifi password, 2 scanner ip, 3 ota password
static KbMode s_kbMode = KB_LOWER;
static String s_kbText;
static bool s_kbShow = false;

// ---------- drawing helpers ----------
static void fitText(const String& txt, int x, int y, int maxW, const lgfx::IFont* font, uint16_t color) {
  s_canvas.setFont(font);
  s_canvas.setTextColor(color);
  String t = txt;
  if (s_canvas.textWidth(t.c_str()) > maxW) {
    while (t.length() > 1 && s_canvas.textWidth((t + "...").c_str()) > maxW) t.remove(t.length() - 1);
    t += "...";
  }
  s_canvas.drawString(t.c_str(), x, y);
}

static void centerText(const String& t, int x, int y, const lgfx::IFont* font, uint16_t color) {
  s_canvas.setFont(font);
  s_canvas.setTextColor(color);
  s_canvas.setTextDatum(textdatum_t::middle_center);
  s_canvas.drawString(t.c_str(), x, y);
  s_canvas.setTextDatum(textdatum_t::top_left);
}

// Scanner-style key: raised charcoal body with a bevel, backlit label with a halo,
// a faint light leak around the edge. Pressed = pushed in and lit up. Active = LED bar.
static const uint16_t K_RIM = 0x0841, K_BODY = 0x2104, K_TOP = 0x2945, K_HI = 0x4208, K_SH = 0x0000;

static void drawButton(const Button& b, bool down) {
  bool armed = b.action == A_REBOOT && rebootArmed();
  int gi = b.glow >= 0 ? b.glow
         : (armed || b.bg == C_BAD || b.bg == C_DARKRED) ? KL_RED
         : b.bg == C_GREENKEY ? KL_GREEN
         : Settings::keyColor();
  const Glow& g = s_glow[constrain(gi, 0, NUM_KEYLIGHTS - 1)];
  int x = b.x, y = b.y, w = b.w, h = b.h, r = 9;

  s_canvas.fillRoundRect(x, y, w, h, r, K_RIM);                       // outer rim
  if (!down) {
    s_canvas.fillRoundRect(x + 1, y + 1, w - 2, h - 2, r - 1, K_BODY);
    s_canvas.fillRoundRect(x + 2, y + 2, w - 4, h / 2, r - 2, K_TOP); // domed upper half
    s_canvas.drawFastHLine(x + r, y + 1, w - 2 * r, K_HI);            // top bevel highlight
    s_canvas.drawFastHLine(x + r, y + h - 2, w - 2 * r, K_SH);        // bottom shadow
  } else {
    s_canvas.fillRoundRect(x + 1, y + 1, w - 2, h - 2, r - 1, g.body);  // pushed in, lit from below
    s_canvas.fillRoundRect(x + 2, y + h / 2, w - 4, h / 2 - 2, r - 2, g.bodyTop);
    s_canvas.drawFastHLine(x + r, y + 1, w - 2 * r, K_SH);             // inverted bevel
  }
  // light leaking around the key edge
  s_canvas.drawRoundRect(x + 1, y + 1, w - 2, h - 2, r - 1, (b.hl || down) ? g.mid : g.faint);
  if (b.hl) {                                                         // active: lit LED bar along the top
    s_canvas.fillRoundRect(x + 10, y + 4, w - 20, 3, 1, g.full);
    s_canvas.drawFastHLine(x + 12, y + 7, w - 24, g.dim);
  }
  if (b.action == A_RECENT_ROW || b.action == A_WIFI_PICK || b.action == A_FAV || b.action == A_SETUP_FIELD) return;   // drawn by the page

  String label = armed ? (b.w > 200 ? String("TAP AGAIN TO REBOOT") : String("TAP AGAIN")) : b.label;
  int cx = x + w / 2, cy = y + h / 2 + (b.hl ? 2 : 0);
  s_canvas.setFont(label.length() == 1 ? (const lgfx::IFont*)&fonts::Font4 : (const lgfx::IFont*)&fonts::Font2);   // big digits / + -
  if (s_canvas.textWidth(label.c_str()) > w - 8) s_canvas.setFont(&fonts::Font0);   // small keys
  s_canvas.setTextDatum(textdatum_t::middle_center);
  uint16_t halo = (b.hl || down) ? g.mid : g.dim;
  s_canvas.setTextColor(halo);
  s_canvas.drawString(label.c_str(), cx - 1, cy);
  s_canvas.drawString(label.c_str(), cx + 1, cy);
  s_canvas.drawString(label.c_str(), cx, cy - 1);
  s_canvas.drawString(label.c_str(), cx, cy + 1);
  s_canvas.setTextColor(down ? C_TEXT : (b.hl ? g.full : g.bright));
  s_canvas.drawString(label.c_str(), cx, cy);
  s_canvas.setTextDatum(textdatum_t::top_left);
}

// Battery icon: 20x12 outline + 2x6 nub at (x, y), percentage in Font0 to its right.
static void drawBattery(int x, int y) {
  int pct = Battery::percent();
  s_canvas.drawRect(x, y, 20, 12, C_TEXT);
  s_canvas.fillRect(x + 20, y + 3, 2, 6, C_TEXT);
  if (pct >= 0) {
    uint16_t c = pct > 40 ? C_OK : pct > 15 ? C_DEPT : C_BAD;
    int fw = (16 * pct + 50) / 100;
    if (fw > 0) s_canvas.fillRect(x + 2, y + 2, fw, 8, c);
  }
  s_canvas.setFont(&fonts::Font0);
  s_canvas.setTextDatum(textdatum_t::middle_left);
  s_canvas.setTextColor(pct >= 0 ? C_TEXT : C_DIM);
  char buf[8];
  if (pct >= 0) snprintf(buf, sizeof(buf), "%d%%", pct); else strcpy(buf, "--");
  s_canvas.drawString(buf, x + 25, y + 6);
  s_canvas.setTextDatum(textdatum_t::top_left);
}

static bool clockText(char* buf, size_t n, time_t t = 0) {
  if (!t) t = time(nullptr);
  if (t < 1700000000) return false;
  struct tm tm;
  localtime_r(&t, &tm);
  int h = tm.tm_hour % 12;
  snprintf(buf, n, "%d:%02d %s", h ? h : 12, tm.tm_min, tm.tm_hour < 12 ? "AM" : "PM");
  return true;
}

static void drawWifiBars(int x, int y, int rssi) {
  int bars = rssi > -55 ? 4 : rssi > -65 ? 3 : rssi > -75 ? 2 : 1;
  for (int i = 0; i < 4; i++) {
    int bh = 4 + i * 4;
    s_canvas.fillRect(x + i * 7, y + 16 - bh, 5, bh, i < bars ? C_TEXT : C_DIM);
  }
}

static void drawLock(int x, int y) {
  s_canvas.drawRoundRect(x + 2, y, 8, 9, 3, C_TEXT);
  s_canvas.fillRect(x, y + 6, 12, 9, C_TEXT);
}

// ---------- MAIN ----------
static void drawStatusBar() {
  const auto& st = Sds200::status();
  s_canvas.fillRect(0, 0, W, 24, C_PANEL);
  s_canvas.setFont(&fonts::Font2);

  // scanner link + clock
  s_canvas.fillCircle(10, 12, 5, st.online ? C_OK : C_BAD);
  char clk[16];
  // the time once the scanner is connected; until then the scanner's name
  String left = (st.online && clockText(clk, sizeof(clk))) ? String(clk) : (st.model.length() ? st.model : String("SDS200"));
  s_canvas.setTextColor(C_TEXT);
  s_canvas.drawString(left.c_str(), 20, 4);

  // audio link
  bool audio = AudioOut::playing();
  uint16_t ac = RtspAudio::connected() ? (audio ? C_OK : C_DIM) : C_BAD;
  s_canvas.fillCircle(100, 12, 5, ac);
  String a = Calls::replaying() ? String("replay") : AudioOut::muted() ? String("muted") : String(RtspAudio::status());

  // board speaker volume (right-aligned), REC to its left while recording
  char buf[24];
  snprintf(buf, sizeof(buf), "SPK %d%%", AudioOut::volume());
  int right = W - 110;
  int levelsLeft = right - s_canvas.textWidth(buf);
  int textEnd = levelsLeft - 8;
  if (Calls::recordingNow()) {
    int rx = levelsLeft - 40;
    s_canvas.fillCircle(rx + 5, 12, 5, C_BAD);
    s_canvas.setTextColor(C_BAD);
    s_canvas.drawString("REC", rx + 13, 4);
    textEnd = rx - 6;
  }
  fitText(a, 110, 4, textEnd - 110, &fonts::Font2, C_DIM);
  s_canvas.setTextColor(C_TEXT);
  s_canvas.setTextDatum(textdatum_t::top_right);
  s_canvas.drawString(buf, right, 4);
  s_canvas.setTextDatum(textdatum_t::top_left);

  drawBattery(W - 104, 6);

  // signal bars (Sig 0-4)
  for (int i = 0; i < 4; i++) {
    int bh = 4 + i * 4;
    uint16_t c = (st.sig > i) ? C_OK : C_DIM;
    s_canvas.fillRect(W - 42 + i * 9, 20 - bh, 6, bh, c);
  }
}

// Main page: one row along the bottom plus SCAN / WX above MUTE / KEYS.
static const int MBY = 264, MBH = 54, MBW = 116, MBG = 4;
#define MCOL(i) (2 + (i) * (MBW + MBG))
static const int WX_TOP = MBY - MBH - MBG, WX_LEFT = MCOL(2);

static void pageMain() {
  const auto& st = Sds200::status();
  drawStatusBar();
  int y = 30;

  if (!st.online) {
    s_canvas.setFont(&fonts::FreeSansBold12pt7b);
    s_canvas.setTextColor(C_BAD);
    s_canvas.drawString("Scanner not responding", 10, 60);
    s_canvas.setFont(&fonts::Font2);
    s_canvas.setTextColor(C_DIM);
    String l = "Looking for " + Settings::scannerIp() + " on UDP " + SCANNER_CTRL_PORT;
    s_canvas.drawString(l.c_str(), 10, 100);
    String w = String("Wi-Fi: ") + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("not connected"));
    s_canvas.drawString(w.c_str(), 10, 120);
    s_canvas.drawString("Scanner IP / Wi-Fi: KEYS > SETTINGS > WI-FI SETUP", 10, 140);
  } else {
    fitText(st.system.length() ? st.system : st.mode, 10, y, 400, &fonts::FreeSans12pt7b, C_SYS);
    if (st.sysHold) { s_canvas.setFont(&fonts::Font2); s_canvas.setTextColor(C_HOLD); s_canvas.drawString("HOLD", 425, y + 4); }
    y += 33;
    fitText(st.department, 10, y, 400, &fonts::FreeSans12pt7b, C_DEPT);
    if (st.deptHold) { s_canvas.setFont(&fonts::Font2); s_canvas.setTextColor(C_HOLD); s_canvas.drawString("HOLD", 425, y + 4); }
    y += 35;
    fitText(st.channel.length() ? st.channel : String("Scanning..."), 10, y, 400, &fonts::FreeSansBold18pt7b,
            st.sig > 0 ? C_TEXT : C_DIM);
    if (st.chanHold) { s_canvas.setFont(&fonts::Font2); s_canvas.setTextColor(C_HOLD); s_canvas.drawString("HOLD", 425, y + 10); }
    y += 48;

    // The scanner's TGID value already includes the word "TGID"; it's "TGID None" or
    // "TGID: ---" when there's no talkgroup.
    String line;
    String tg = st.tgid; tg.trim();
    if (tg.length() && tg.indexOf("None") < 0 && tg.indexOf("---") < 0) line += tg + "   ";
    if (st.freq.length()) { String f = st.freq; f.trim(); line += f + "   "; }
    if (st.mod.length()) line += st.mod;
    auto lineW = [](int y, int h) { return (y + h > WX_TOP) ? WX_LEFT - 18 : 460; };
    fitText(line, 10, y, lineW(y, 26), &fonts::Font4, C_TEXT);
    y += 30;
    if (st.service.length()) { fitText(st.service, 10, y, lineW(y, 26), &fonts::Font4, C_DIM); y += 30; }
    String sub;
    if (st.site.length()) sub += "Site: " + st.site + "   ";
    String rssi = st.rssi; rssi.trim();
    if (rssi.length() && rssi != "-999") sub += "RSSI " + rssi + "   ";
    if (st.info1.length()) sub += st.info1;
    if (sub.length()) { fitText(sub, 10, y, lineW(y, 16), &fonts::Font2, C_DIM); y += 20; }
    if (st.info2.length() && y < 244) fitText(st.info2, 10, y, lineW(y, 16), &fonts::Font2, C_DIM);
  }

  add(MCOL(0), MBY, MBW, MBH, "SPK -", A_SPK_DN);
  add(MCOL(1), MBY, MBW, MBH, "SPK +", A_SPK_UP);
  add(MCOL(2), MBY, MBW, MBH, "MUTE", A_SPK_MUTE, 0, AudioOut::muted());
  add(MCOL(3), MBY, MBW, MBH, "KEYS", A_GOTO, PAGE_KEYS);
  add(MCOL(2), WX_TOP, MBW, MBH, "SCAN", A_SCAN, 0, st.online && !Sds200::inWeather());
  if (RtspAudio::lockedOut() && st.online)   // scanner answers but refuses audio: old listener stuck
    add(MCOL(0), WX_TOP, 2 * MBW + MBG, MBH, rebootArmed() ? "TAP AGAIN TO REBOOT" : "AUDIO STUCK: REBOOT SCANNER", A_REBOOT, 0, false, C_DARKRED);
  add(MCOL(3), WX_TOP, MBW, MBH, "WX", A_WX, 0, st.online && Sds200::inWeather());

  if (st.popup.length()) {
    s_canvas.fillRoundRect(60, 70, 360, 90, 10, C_PANEL);
    s_canvas.drawRoundRect(60, 70, 360, 90, 10, C_DEPT);
    centerText(st.popup, 240, 105, &fonts::FreeSans12pt7b, C_TEXT);
    centerText("KEYS page: E/YES to accept", 240, 140, &fonts::Font2, C_DIM);
  }
}

// ---------- KEYS ----------
static const int KW = 76, KH = 55, KG = 3, KY = 28;
#define KX(i) (2 + (i) * (KW + KG + 1))
#define KYR(r) (KY + (r) * (KH + KG))

static void header(const String& title, const String& right = "") {
  s_canvas.fillRect(0, 0, W, 26, C_PANEL);
  fitText(title, 6, 5, right.length() ? 250 : 400, &fonts::Font2, C_TEXT);
  if (right.length()) fitText(right, 262, 5, 150, &fonts::Font2, C_DIM);
  drawBattery(W - 56, 7);
}

static void pageKeys() {
  const auto& st = Sds200::status();
  char buf[32];
  snprintf(buf, sizeof(buf), "SPK %d%%%s", AudioOut::volume(), AudioOut::muted() ? " (muted)" : "");
  if (st.popup.length()) header(st.channel.length() ? st.channel : st.mode, st.popup);
  else header(st.channel.length() ? st.channel : st.mode, buf);

  static const char* pad = "123456789.0E";
  for (int i = 0; i < 12; i++) {
    char k = pad[i];
    add(KX(i % 3), KYR(i / 3), KW, KH, k == 'E' ? String("E/YES") : String(k), A_KEY, k);
  }
  struct { const char* l; char k; } right[] = {
    {"SYSTEM", 'A'}, {"DEPT", 'B'}, {"CHAN", 'C'}, {"<", '<'}, {"PUSH", '^'}, {">", '>'},
    {"MENU", 'M'}, {"FUNC", 'F'}, {"AVOID", 'L'}, {"ZIP", 'Z'}, {"RANGE", 'R'}, {"REPLAY", 'Y'},
  };
  for (int i = 0; i < 12; i++) {
    bool hl = (right[i].k == 'A' && st.sysHold) || (right[i].k == 'B' && st.deptHold) || (right[i].k == 'C' && st.chanHold);
    add(KX(3 + i % 3), KYR(i / 3), KW, KH, right[i].l, A_KEY, right[i].k, hl);
  }
  bool rec = Settings::get().record;
  add(KX(0), KYR(4), KW, KH, rec ? "REC ON" : "REC OFF", A_REC, 0, rec, rec ? C_DARKRED : C_BTN);
  add(KX(1), KYR(4), KW, KH, "RECENT", A_GOTO, PAGE_RECENT);
  add(KX(2), KYR(4), KW, KH, "FAVS", A_GOTO, PAGE_FAVS);
  add(KX(3), KYR(4), KW, KH, "SETTINGS", A_GOTO, PAGE_SETTINGS);
  add(KX(4), KYR(4), KW, KH, "POWER", A_POWER_MENU);
  add(KX(5), KYR(4), KW, KH, "BACK", A_GOTO, PAGE_MAIN);
}

// ---------- POWER ----------
static void pagePower() {
  centerText("Power", W / 2, 50, &fonts::FreeSansBold18pt7b, C_TEXT);
  centerText(Board::hasBootButton() ? "After POWER OFF, press BOOT to turn it back on"
                                    : "After POWER OFF, press the power button to turn it back on",
             W / 2, 85, &fonts::Font2, C_DIM);
  if (Board::hasLauncher()) {
    add(8, 120, 110, 120, "POWER OFF", A_POWER_OFF, 0, false, C_BAD);
    add(126, 120, 110, 120, "RESTART", A_RESTART);
    add(244, 120, 110, 120, "LAUNCHER", A_LAUNCHER);
    add(362, 120, 110, 120, "CANCEL", A_POWER_CANCEL);
  } else {
    add(16, 120, 144, 120, "POWER OFF", A_POWER_OFF, 0, false, C_BAD);
    add(168, 120, 144, 120, "RESTART", A_RESTART);
    add(320, 120, 144, 120, "CANCEL", A_POWER_CANCEL);
  }
}

// ---------- SETTINGS ----------
static const uint16_t kDimAfter[] = {0, 30, 60, 120, 300, 600};
static const uint8_t kDimLevel[] = {0, 5, 10, 20, 30, 50};

static String dimAfterText(uint16_t s) {
  if (!s) return "Never";
  if (s < 60) return String(s) + " s";
  return String(s / 60) + " min";
}

static void pageSettings() {
  header("Settings");
  const auto& c = Settings::get();
  struct { const char* label; String value; } rows[] = {
    {"Brightness", String(c.brightness) + "%"},
    {"Dim after", dimAfterText(c.dimAfterS)},
    {"Dimmed level", c.dimLevel ? String(c.dimLevel) + "%" : String("Off")},
  };
  for (int r = 0; r < 3; r++) {
    int y = 30 + r * 46;
    fitText(rows[r].label, 12, y + 10, 220, &fonts::Font4, C_TEXT);
    add(250, y, 60, 42, "-", A_ADJ, r * 2);
    centerText(rows[r].value, 355, y + 21, &fonts::Font4, C_DEPT);
    add(400, y, 60, 42, "+", A_ADJ, r * 2 + 1);
  }

  // Key light: sample keys, tap one to use it
  fitText("Key light", 12, 170, 200, &fonts::Font2, C_TEXT);
  fitText(kKeyLights[Settings::keyColor()].name, 90, 170, 200, &fonts::Font2, s_glow[Settings::keyColor()].full);
  for (int i = 0; i < NUM_KEYLIGHTS; i++)
    add(2 + i * 48, 188, 44, 40, kKeyLights[i].name, A_KEYLIGHT, i, i == Settings::keyColor(), C_BTN, i);

  int y = 232;
  String wifi = WiFi.status() == WL_CONNECTED ? "Wi-Fi: " + WiFi.SSID() + "  IP " + WiFi.localIP().toString()
                                              : String("Wi-Fi: not connected");
  fitText(wifi + "   Scanner: " + Settings::scannerIp(), 12, y, 456, &fonts::Font0, C_DIM); y += 12;
  String sd = Calls::sdInfo() + (Calls::sdOk() ? (Settings::get().record ? ", recording on" : ", recording off") : String(""));
  String upd = Board::hasLauncher() ? String("via Launcher") : Ota::enabled() ? String(OTA_HOSTNAME) + ".local" : String("off");
  fitText(sd + "   Updates: " + upd, 12, y, 456, &fonts::Font0, C_DIM);

  add(2, 262, 116, 54, "WI-FI SETUP", A_GOTO, PAGE_SETUP);
  add(122, 262, 138, 54, "RECONNECT AUDIO", A_AUDIO_RECONNECT);
  add(264, 262, 134, 54, "REBOOT SCANNER", A_REBOOT);
  add(402, 262, 76, 54, "BACK", A_GOTO, PAGE_KEYS);
}

// ---------- RECENT ----------
static const int ROWS = 5, ROW_H = 44, ROW_Y = 30;

static void pageRecent() {
  int n = Calls::recentCount();
  header("Recent calls (" + String(n) + ")", Calls::sdOk() ? String("tap to replay") : String("no SD card"));
  if (s_recentTop > max(0, n - ROWS)) s_recentTop = max(0, n - ROWS);
  if (!Calls::replaying()) s_playingRow = -1;
  if (n == 0) centerText("No calls yet", W / 2, 140, &fonts::Font4, C_DIM);
  for (int r = 0; r < ROWS; r++) {
    int i = s_recentTop + r;
    Calls::Call c;
    if (!Calls::recent(i, c)) break;
    int y = ROW_Y + r * (ROW_H + 2);
    add(2, y, 476, ROW_H, "", A_RECENT_ROW, i, i == s_playingRow);
    drawButton(s_btns.back(), false);
    char t[16] = "";
    if (!clockText(t, sizeof(t), c.start)) snprintf(t, sizeof(t), "--:--");
    fitText(t, 8, y + 4, 70, &fonts::Font2, C_DIM);
    fitText(c.channel.length() ? c.channel : c.tgid, 78, y + 4, 320, &fonts::Font2, C_TEXT);
    fitText(c.dept.length() ? c.dept : c.system, 78, y + 24, 320, &fonts::Font0, C_DIM);
    char d[16];
    snprintf(d, sizeof(d), "%lus", (unsigned long)((c.durMs + 500) / 1000));
    fitText(d, 404, y + 4, 40, &fonts::Font2, C_DIM);
    if (c.wav.length()) {
      if (i == s_playingRow) s_canvas.fillRect(450, y + 14, 14, 14, C_OK);
      else s_canvas.fillTriangle(450, y + 12, 450, y + 32, 466, y + 22, C_OK);
    }
  }
  add(2, 264, 116, 54, "UP", A_LIST_UP);
  add(122, 264, 116, 54, "DOWN", A_LIST_DN);
  add(242, 264, 116, 54, "STOP", A_STOP_PLAY, 0, Calls::replaying());
  add(362, 264, 116, 54, "BACK", A_GOTO, PAGE_KEYS);
}

// ---------- FAVS ----------
static void pageFavs() {
  header("Favorites: tap = hold there, hold 1 s = save current");
  const auto& c = Settings::get();
  for (int i = 0; i < Settings::NUM_FAVS; i++) {
    int x = 2 + (i % 2) * 240, y = 30 + (i / 2) * 76;
    add(x, y, 236, 72, "", A_FAV, i);
    drawButton(s_btns.back(), false);
    const auto& f = c.favs[i];
    if (f.used) {
      fitText(strlen(f.dept) ? f.dept : f.sys, x + 10, y + 12, 216, &fonts::Font2, C_TEXT);
      fitText(strlen(f.dept) ? f.sys : "(whole system)", x + 10, y + 40, 216, &fonts::Font2, C_DIM);
    } else {
      fitText(String("Slot ") + (i + 1) + " - empty", x + 10, y + 12, 216, &fonts::Font2, C_DIM);
      fitText("hold to save current", x + 10, y + 40, 216, &fonts::Font2, C_DIM);
    }
  }
  add(2, 264, 236, 54, "SCAN (release hold)", A_SCAN);
  add(242, 264, 236, 54, "BACK", A_GOTO, PAGE_KEYS);
}

// ---------- SETUP ----------
static void loadSetupFields() {
  s_eSsid = Settings::wifiSsid();
  s_ePass = Settings::wifiPassword();
  s_eIp = Settings::scannerIp();
  s_eOta = Settings::otaPassword();
  s_setupMsg = "";
}

static void pageSetup() {
  header(s_firstRun ? "Welcome! Set up Wi-Fi and the scanner" : "Wi-Fi and scanner setup");
  struct { const char* label; String value; } rows[] = {
    {"Wi-Fi network", s_eSsid.length() ? s_eSsid : String("(tap to choose)")},
    {"Wi-Fi password", s_ePass.length() ? String("********") : String("(none)")},
    {"Scanner IP", s_eIp},
    {"Update password", s_eOta.length() ? String("********") : String("(off)")},
  };
  for (int r = 0; r < 4; r++) {
    int y = 32 + r * 48;
    fitText(rows[r].label, 10, y + 13, 150, &fonts::Font2, C_TEXT);
    add(160, y, 318, 44, "", A_SETUP_FIELD, r);
    drawButton(s_btns.back(), false);
    fitText(rows[r].value, 172, y + 13, 296, &fonts::Font2, C_DEPT);
  }
  fitText(s_setupMsg, 10, 232, 460, &fonts::Font2, s_setupMsg.startsWith("Connected") || s_setupMsg.startsWith("Saved") ? C_OK : C_HOLD);
  add(2, 262, 200, 54, "SAVE & CONNECT", A_SAVE_CONNECT, 0, false, C_GREENKEY);
  add(206, 262, 150, 54, "FORGET WI-FI", A_FORGET_WIFI);
  add(360, 262, 118, 54, "BACK", A_GOTO, PAGE_SETTINGS);
}

static void scanWifi() {
  showMessage("Looking for Wi-Fi networks...");
  s_nets.clear();
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;
    bool dup = false;
    for (auto& x : s_nets) if (x.ssid == ssid) { dup = true; x.rssi = max(x.rssi, (int)WiFi.RSSI(i)); }
    if (!dup) s_nets.push_back({ssid, WiFi.RSSI(i), WiFi.encryptionType(i) == WIFI_AUTH_OPEN});
  }
  WiFi.scanDelete();
  std::sort(s_nets.begin(), s_nets.end(), [](const Net& a, const Net& b) { return a.rssi > b.rssi; });
  s_netTop = 0;
  s_dirty = true;
}

static void pageWifi() {
  header("Choose your Wi-Fi network (2.4 GHz)", String(s_nets.size()) + " found");
  if (s_nets.empty()) centerText("No networks found - tap RESCAN", W / 2, 140, &fonts::Font2, C_DIM);
  if (s_netTop > max(0, (int)s_nets.size() - ROWS)) s_netTop = max(0, (int)s_nets.size() - ROWS);
  for (int r = 0; r < ROWS; r++) {
    int i = s_netTop + r;
    if (i >= (int)s_nets.size()) break;
    int y = ROW_Y + r * (ROW_H + 2);
    add(2, y, 476, ROW_H, "", A_WIFI_PICK, i, s_nets[i].ssid == s_eSsid);
    drawButton(s_btns.back(), false);
    fitText(s_nets[i].ssid, 12, y + 14, 360, &fonts::Font2, C_TEXT);
    if (!s_nets[i].open) drawLock(410, y + 14);
    drawWifiBars(436, y + 12, s_nets[i].rssi);
  }
  add(2, 264, 92, 54, "UP", A_LIST_UP);
  add(98, 264, 92, 54, "DOWN", A_LIST_DN);
  add(194, 264, 92, 54, "OTHER...", A_WIFI_OTHER);
  add(290, 264, 92, 54, "RESCAN", A_WIFI_RESCAN);
  add(386, 264, 92, 54, "BACK", A_GOTO, PAGE_SETUP);
}

// ---------- on-screen keyboard ----------
static const char* kbRows(KbMode m, int r) {
  static const char* lower[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
  static const char* upper[] = {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"};
  static const char* num[] = {"1234567890", "-/:;()$&@\"", ".,?!'#%"};
  static const char* sym[] = {"[]{}^*+=<>", "_\\|~`", ".,?!'#%"};
  switch (m) {
    case KB_UPPER: return upper[r];
    case KB_NUM: return num[r];
    case KB_SYM: return sym[r];
    default: return lower[r];
  }
}

static void openKeyboard(int field) {
  s_kbField = field;
  s_kbText = field == 0 ? s_eSsid : field == 1 ? s_ePass : field == 2 ? s_eIp : s_eOta;
  s_kbMode = field == 2 ? KB_IP : KB_LOWER;
  s_kbShow = (field == 0 || field == 2);
  s_page = PAGE_KB;
}

static void pageKb() {
  static const char* titles[] = {"Wi-Fi network name", "Wi-Fi password", "Scanner IP address", "Update (OTA) password"};
  bool secret = (s_kbField == 1 || s_kbField == 3) && !s_kbShow;
  s_canvas.setFont(&fonts::Font2);
  s_canvas.setTextColor(C_DIM);
  s_canvas.drawString(titles[s_kbField], 8, 2);
  s_canvas.fillRoundRect(4, 20, 472, 32, 6, C_PANEL);
  String shown = secret ? String() : s_kbText;
  if (secret) for (size_t i = 0; i < s_kbText.length(); i++) shown += '*';
  s_canvas.setFont(&fonts::Font4);
  while (shown.length() && s_canvas.textWidth(shown.c_str()) > 440) shown.remove(0, 1);
  s_canvas.setTextColor(C_TEXT);
  s_canvas.drawString(shown.c_str(), 12, 24);
  int cx = 12 + s_canvas.textWidth(shown.c_str()) + 2;
  s_canvas.fillRect(cx, 24, 2, 24, C_HOLD);

  const int KH2 = 46, R0 = 58, RS = 52;
  if (s_kbMode == KB_IP) {
    static const char* ip[] = {"123", "456", "789", ".0"};
    for (int r = 0; r < 4; r++)
      for (int c = 0; ip[r][c]; c++) add(12 + c * 154, R0 + r * RS, 150, KH2, String(ip[r][c]), A_KB_CHAR, ip[r][c]);
    add(12 + 2 * 154, R0 + 3 * RS, 150, KH2, "DEL", A_KB_BKSP);
  } else {
    for (int r = 0; r < 3; r++) {
      const char* row = kbRows(s_kbMode, r);
      int n = strlen(row);
      int x0 = (r == 2) ? 74 : (W - n * 48) / 2;
      for (int c = 0; c < n; c++) add(x0 + c * 48, R0 + r * RS, 46, KH2, String(row[c]), A_KB_CHAR, row[c]);
    }
    bool letters = s_kbMode == KB_LOWER || s_kbMode == KB_UPPER;
    add(2, R0 + 2 * RS, 68, KH2, letters ? "SHIFT" : (s_kbMode == KB_NUM ? "#+=" : "123"), A_KB_SHIFT, 0, s_kbMode == KB_UPPER);
    add(410, R0 + 2 * RS, 68, KH2, "DEL", A_KB_BKSP);
    add(2, R0 + 3 * RS, 96, KH2, letters ? "123" : "ABC", A_KB_MODE);
    add(102, R0 + 3 * RS, 244, KH2, "space", A_KB_CHAR, ' ');
    if (s_kbField == 1 || s_kbField == 3) add(350, R0 + 3 * RS, 128, KH2, s_kbShow ? "HIDE" : "SHOW", A_KB_SHOW);
  }
  add(2, 266, 236, 50, "CANCEL", A_KB_CANCEL);
  add(242, 266, 236, 50, "OK", A_KB_OK, 0, false, C_GREENKEY);
}

// ---------- weather alert overlay ----------
static void drawAlert() {
  bool on = (millis() / 500) % 2;
  uint16_t bg = on ? C_BAD : C_DARKRED;
  s_canvas.fillRoundRect(10, 50, 460, 170, 12, bg);
  s_canvas.drawRoundRect(10, 50, 460, 170, 12, C_TEXT);
  centerText("WEATHER ALERT", W / 2, 90, &fonts::FreeSansBold18pt7b, C_TEXT);
  const auto& st = Sds200::status();
  String f = st.freq; f.trim();
  String l = (st.channel.length() ? st.channel : String("NOAA Weather")) + (f.length() ? "   " + f : String(""));
  centerText(l, W / 2, 140, &fonts::FreeSans12pt7b, C_TEXT);
  centerText("Tap anywhere to dismiss", W / 2, 190, &fonts::Font2, C_TEXT);
}

// ---------- redraw ----------
static void redraw() {
  s_canvas.fillScreen(C_BG);
  s_btns.clear();
  switch (s_page) {
    case PAGE_MAIN: pageMain(); break;
    case PAGE_KEYS: pageKeys(); break;
    case PAGE_POWER: pagePower(); break;
    case PAGE_SETTINGS: pageSettings(); break;
    case PAGE_RECENT: pageRecent(); break;
    case PAGE_FAVS: pageFavs(); break;
    case PAGE_SETUP: pageSetup(); break;
    case PAGE_WIFI: pageWifi(); break;
    case PAGE_KB: pageKb(); break;
  }
  for (size_t i = 0; i < s_btns.size(); i++) {
    const Button& b = s_btns[i];
    bool custom = b.action == A_RECENT_ROW || b.action == A_WIFI_PICK || b.action == A_FAV || b.action == A_SETUP_FIELD;
    if (custom) {                                   // already drawn under its text; only show the press
      if ((int)i == s_pressed) s_canvas.drawRoundRect(b.x + 1, b.y + 1, b.w - 2, b.h - 2, 8, s_glow[Settings::keyColor()].full);
      continue;
    }
    drawButton(b, (int)i == s_pressed);
  }
  if (alertShowing()) drawAlert();
  if (s_toast.length() && millis() < s_toastUntil) {
    s_canvas.fillRoundRect(60, 150, 360, 40, 8, C_BTN_DOWN);
    centerText(s_toast, 240, 170, &fonts::Font4, C_TEXT);
  }
  Board::present();
}

// ---------- actions ----------
static void adjustSetting(int which, int dir) {
  auto& c = Settings::get();
  if (which == 0) c.brightness = constrain(c.brightness + dir * 10, 10, 100);
  if (which == 1) {
    int i = 0;
    while (i < 5 && kDimAfter[i] != c.dimAfterS) i++;
    c.dimAfterS = kDimAfter[constrain(i + dir, 0, 5)];
  }
  if (which == 2) {
    int i = 0;
    while (i < 5 && kDimLevel[i] != c.dimLevel) i++;
    c.dimLevel = kDimLevel[constrain(i + dir, 0, 5)];
  }
  Settings::changed();
}

static void saveAndConnect() {
  IPAddress ip;
  if (!ip.fromString(s_eIp) || ip == IPAddress(0, 0, 0, 0)) { s_setupMsg = "That scanner IP isn't valid"; return; }
  if (s_eIp != Settings::scannerIp()) {
    Settings::setScannerIp(s_eIp);
    Sds200::setIp(s_eIp);
    RtspAudio::reconnect();
  }
  if (!s_eSsid.length()) { s_setupMsg = "Saved scanner IP. Choose a Wi-Fi network too."; return; }

  bool same = s_eSsid == Settings::wifiSsid() && s_ePass == Settings::wifiPassword() && WiFi.status() == WL_CONNECTED;
  if (!same) {
    String oldSsid = Settings::wifiSsid(), oldPass = Settings::wifiPassword();
    showMessage("Connecting to Wi-Fi", s_eSsid.c_str());
    WiFi.disconnect();
    delay(200);
    WiFi.begin(s_eSsid.c_str(), s_ePass.c_str());
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) delay(100);
    if (WiFi.status() != WL_CONNECTED) {
      s_setupMsg = "Couldn't join \"" + s_eSsid + "\" - check the password (nothing saved)";
      WiFi.disconnect();
      if (oldSsid.length()) WiFi.begin(oldSsid.c_str(), oldPass.c_str());
      return;
    }
    Settings::setWifi(s_eSsid, s_ePass);
  }
  if (s_eOta != Settings::otaPassword()) { Settings::setOtaPassword(s_eOta); Ota::restart(); }
  s_setupMsg = (same ? String("Saved") : String("Connected")) + " - board IP " + WiFi.localIP().toString();
  s_firstRun = false;
}

static void goPage(Page p) {
  if (p == PAGE_SETUP && s_page != PAGE_SETUP && s_page != PAGE_WIFI && s_page != PAGE_KB) loadSetupFields();
  if (p == PAGE_RECENT && s_page != PAGE_RECENT) s_recentTop = 0;
  if (s_page == PAGE_RECENT && p != PAGE_RECENT && Calls::replaying()) Calls::stopPlay();
  s_page = p;
}

static void doAction(const Button& b, bool longPress) {
  const auto& st = Sds200::status();
  switch (b.action) {
    case A_KEY: Sds200::key((char)b.arg, longPress ? 'L' : 'P'); break;
    case A_SPK_DN:
    case A_SPK_UP:
      AudioOut::setVolume(AudioOut::volume() + (b.action == A_SPK_UP ? 10 : -10));
      Settings::get().spkVolume = AudioOut::volume();
      Settings::changed();
      toast(String("Speaker ") + AudioOut::volume() + "%");
      break;
    case A_SPK_MUTE:
      AudioOut::setMuted(!AudioOut::muted());
      Settings::get().muted = AudioOut::muted();
      Settings::changed();
      break;
    case A_GOTO: goPage((Page)b.arg); break;
    case A_POWER_CANCEL: s_page = s_pageBeforePower; break;
    case A_POWER_MENU: s_pageBeforePower = s_page; s_page = PAGE_POWER; break;
    case A_LAUNCHER:
      showLogo("Returning to Launcher...");
      Calls::closeAll();
      Settings::saveNow();
      RtspAudio::shutdown();
      Board::exitToLauncher();
      break;
    case A_POWER_OFF:
      showLogo("Powering off - press BOOT to turn on");
      Calls::closeAll();
      Settings::saveNow();
      delay(600);
      Power::off();
      break;
    case A_RESTART:
      showLogo("Restarting...");
      Calls::closeAll();
      Settings::saveNow();
      Power::restart();
      break;
    case A_WX: Sds200::weather(true); toast("Weather"); break;
    case A_SCAN: Sds200::weather(false); toast("Scanning"); if (s_page == PAGE_FAVS) s_page = PAGE_MAIN; break;
    case A_AUDIO_RECONNECT: RtspAudio::reconnect(); toast("Reconnecting audio"); break;
    case A_REBOOT:
      if (rebootArmed()) {
        s_rebootArmedUntil = 0;
        Sds200::rebootScanner();
        RtspAudio::reconnect();
        toast("Rebooting scanner");
      } else {
        s_rebootArmedUntil = millis() + REBOOT_CONFIRM_MS;
      }
      break;
    case A_REC: {
      auto& c = Settings::get();
      c.record = !c.record;
      Settings::changed();
      toast(!Calls::sdOk() ? String("No SD card") : c.record ? String("Recording on") : String("Recording off"));
      break;
    }
    case A_RECENT_ROW:
      if (s_playingRow == b.arg && Calls::replaying()) { Calls::stopPlay(); s_playingRow = -1; }
      else if (Calls::play(b.arg)) s_playingRow = b.arg;
      else toast(Calls::sdOk() ? "Not recorded" : "No SD card");
      break;
    case A_STOP_PLAY: Calls::stopPlay(); s_playingRow = -1; break;
    case A_LIST_UP:
      if (s_page == PAGE_RECENT) s_recentTop = max(0, s_recentTop - ROWS);
      else s_netTop = max(0, s_netTop - ROWS);
      break;
    case A_LIST_DN:
      if (s_page == PAGE_RECENT) s_recentTop += ROWS;
      else s_netTop += ROWS;
      break;
    case A_FAV: {
      auto& f = Settings::get().favs[b.arg];
      if (longPress) {
        if (!st.online || Sds200::inWeather() || st.sysIndex == 0xFFFFFFFF || !st.system.length()) { toast("Nothing to save"); break; }
        f.used = true;
        f.sysIndex = st.sysIndex;
        bool dept = st.deptIndex != 0xFFFFFFFF && st.department.length();
        f.deptIndex = dept ? st.deptIndex : 0xFFFFFFFF;
        strlcpy(f.sys, st.system.c_str(), sizeof(f.sys));
        strlcpy(f.dept, dept ? st.department.c_str() : "", sizeof(f.dept));
        Settings::changed();
        toast("Saved to slot " + String(b.arg + 1));
      } else if (f.used) {
        Sds200::holdOn(f.sysIndex, f.deptIndex);
        toast(String("Holding: ") + (strlen(f.dept) ? f.dept : f.sys));
        s_page = PAGE_MAIN;
      } else {
        toast("Hold for 1 s to save here");
      }
      break;
    }
    case A_ADJ: adjustSetting(b.arg / 2, (b.arg % 2) ? 1 : -1); break;
    case A_KEYLIGHT: Settings::setKeyColor(b.arg); break;
    case A_SETUP_FIELD:
      if (b.arg == 0) { scanWifi(); s_page = PAGE_WIFI; }
      else openKeyboard(b.arg);
      break;
    case A_WIFI_PICK: {
      const Net& n = s_nets[b.arg];
      if (n.ssid != s_eSsid) s_ePass = "";
      s_eSsid = n.ssid;
      if (n.open) { s_ePass = ""; s_page = PAGE_SETUP; }
      else openKeyboard(1);
      break;
    }
    case A_WIFI_OTHER: openKeyboard(0); break;
    case A_WIFI_RESCAN: scanWifi(); break;
    case A_SAVE_CONNECT: saveAndConnect(); break;
    case A_FORGET_WIFI:
      Settings::forgetWifi();
      s_eSsid = ""; s_ePass = "";
      WiFi.disconnect();
      s_setupMsg = "Wi-Fi forgotten";
      break;
    case A_KB_CHAR:
      if (s_kbText.length() < 63) s_kbText += (char)b.arg;
      if (s_kbMode == KB_UPPER) s_kbMode = KB_LOWER;     // shift is one-shot
      break;
    case A_KB_BKSP: if (s_kbText.length()) s_kbText.remove(s_kbText.length() - 1); break;
    case A_KB_SHIFT:
      s_kbMode = s_kbMode == KB_LOWER ? KB_UPPER : s_kbMode == KB_UPPER ? KB_LOWER : s_kbMode == KB_NUM ? KB_SYM : KB_NUM;
      break;
    case A_KB_MODE: s_kbMode = (s_kbMode == KB_LOWER || s_kbMode == KB_UPPER) ? KB_NUM : KB_LOWER; break;
    case A_KB_SHOW: s_kbShow = !s_kbShow; break;
    case A_KB_CANCEL: s_page = PAGE_SETUP; break;
    case A_KB_OK:
      if (s_kbField == 0) { s_eSsid = s_kbText; s_ePass = ""; openKeyboard(1); break; }
      if (s_kbField == 1) s_ePass = s_kbText;
      if (s_kbField == 2) { s_eIp = s_kbText; s_eIp.trim(); }
      if (s_kbField == 3) s_eOta = s_kbText;
      s_page = PAGE_SETUP;
      break;
    default: break;
  }
  s_dirty = true;
}

// ---------- touch ----------
static int hitTest(int x, int y) {
  for (size_t i = 0; i < s_btns.size(); i++) {
    const Button& b = s_btns[i];
    if (x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h) return i;
  }
  return -1;
}

static void handleTouch() {
  int sx, sy, lx = -1, ly = -1;
  bool down = Board::touch(sx, sy);
  if (down) {                                    // canvas pixels -> logical 480x320
    lx = (int)(sx / s_canvas.kx);
    ly = (int)(sy / s_canvas.ky);
    if (lx < 0 || lx >= W || ly < 0 || ly >= H) down = false;
  }

  // A touch on a dimmed screen (or on the weather alert) is used up by waking / dismissing.
  if (down && !s_swallowTouch && s_pressed < 0) {
    bool wasDim = s_dimmed;
    activity();
    if (wasDim) { s_dimmed = false; s_swallowTouch = true; s_dirty = true; }
    else if (alertShowing()) {
      Calls::ackAlert();
      if (gsiAlert()) s_gsiAlertAck = true;
      s_swallowTouch = true;
      s_dirty = true;
    }
  }
  if (s_swallowTouch) { if (!down) s_swallowTouch = false; return; }
  if (down) activity();

  if (down && s_pressed < 0) {
    int hit = hitTest(lx, ly);
    if (hit >= 0) { s_pressed = hit; s_pressStart = millis(); s_longSent = false; s_dirty = true; }
  } else if (down && s_pressed >= 0) {
    if (s_pressed >= (int)s_btns.size()) { s_pressed = -1; return; }
    const Button b = s_btns[s_pressed];
    if (!s_longSent && (b.action == A_KEY || b.action == A_FAV) && millis() - s_pressStart > (b.action == A_FAV ? 1000u : 700u)) {
      s_longSent = true;
      s_pressed = -1;
      doAction(b, true);
      if (b.action == A_KEY) toast("Long press");
    }
  } else if (!down && s_pressed >= 0) {
    if (s_pressed >= (int)s_btns.size()) { s_pressed = -1; return; }
    const Button b = s_btns[s_pressed];
    s_pressed = -1;
    if (!s_longSent) doAction(b, false);
    s_dirty = true;
  } else if (!down) {
    s_longSent = false;
  }
}

// ---------- setup ----------
bool begin() {
  if (!Board::beginDisplay()) return false;
  LGFX_Sprite& c = Board::canvas();
  s_canvas.c = &c;
  s_canvas.kx = c.width() / (float)W;
  s_canvas.ky = c.height() / (float)H;
  initGlows();
  s_lastActivity = millis();
  return true;
}

void openSetup(bool firstRun) {
  s_firstRun = firstRun;
  loadSetupFields();
  s_page = PAGE_SETUP;
  s_dirty = true;
}

static void drawBootStatus(const char* status) {
  if (!status || !*status) return;
  s_canvas.setFont(&fonts::Font2);
  s_canvas.setTextDatum(textdatum_t::bottom_center);
  s_canvas.setTextColor(C_DIM);
  s_canvas.drawString(status, W / 2, H - 6);
  s_canvas.setTextDatum(textdatum_t::top_left);
}

void showLogo(const char* status) {
  s_canvas.fillScreen(C_BG);
  s_canvas.drawJpg(kLogoStart, kLogoEnd - kLogoStart, 0, 0);
  drawBootStatus(status);
  Board::present();
}

// boot_anim.bin: "UCA1", u16 frames, u16 ms/frame, then {u32 offset, u32 length} per frame, then JPEGs.
void bootAnimFrame(const char* status) {
  static uint16_t frame = 0;
  static uint32_t nextAt = 0;
  const uint8_t* a = kAnimStart;
  if (memcmp(a, "UCA1", 4) != 0) { showLogo(status); delay(100); return; }
  uint16_t count = a[4] | (a[5] << 8);
  uint16_t ms = a[6] | (a[7] << 8);
  const uint8_t* e = a + 8 + 8 * frame;
  uint32_t off = e[0] | (e[1] << 8) | (e[2] << 16) | ((uint32_t)e[3] << 24);
  uint32_t len = e[4] | (e[5] << 8) | (e[6] << 16) | ((uint32_t)e[7] << 24);
  if (!s_canvas.drawJpg(a + off, len, 0, 0)) s_canvas.drawJpg(kLogoStart, kLogoEnd - kLogoStart, 0, 0);
  drawBootStatus(status);
  int32_t wait = (int32_t)(nextAt - millis());
  if (wait > 0 && wait <= ms) delay(wait);
  Board::present();
  nextAt = millis() + ms;
  frame = (frame + 1) % count;
}

void showMessage(const char* line1, const char* line2) {
  s_canvas.fillScreen(C_BG);
  s_canvas.setTextDatum(textdatum_t::middle_center);
  s_canvas.setFont(&fonts::FreeSansBold18pt7b);
  s_canvas.setTextColor(C_SYS);
  s_canvas.drawString("Uniden Connect", W / 2, 110);
  s_canvas.setFont(&fonts::FreeSans12pt7b);
  s_canvas.setTextColor(C_TEXT);
  if (line1) s_canvas.drawString(line1, W / 2, 170);
  s_canvas.setFont(&fonts::Font2);
  s_canvas.setTextColor(C_DIM);
  if (line2) s_canvas.drawString(line2, W / 2, 205);
  s_canvas.setTextDatum(textdatum_t::top_left);
  Board::present();
}

// Serial debug helper: "S" sends a screenshot (raw RGB565 of the 320x480 frame buffer),
// "P<n>" shows page n. Used for checking layouts from a computer.
static void serialDebug() {
  if (!Serial.available()) return;
  int c = Serial.read();
  if (c == 'S') {
    LGFX_Sprite& raw = Board::canvas();
    const uint8_t* p = (const uint8_t*)raw.getBuffer();
    size_t left = raw.bufferLength();
    if (s_canvas.scaled()) {                     // "SHT2", width, height (u16 LE), then pixels
      uint16_t wh[2] = {(uint16_t)raw.width(), (uint16_t)raw.height()};
      Serial.write((const uint8_t*)"SHT2", 4);
      Serial.write((const uint8_t*)wh, 4);
    } else {
      Serial.write((const uint8_t*)"SHOT", 4);   // 320x480 panel-order frame
    }
#if ARDUINO_USB_MODE
    Serial.setTxTimeoutMs(2000);                 // wait for the computer instead of dropping data
#endif
    while (left) {
      size_t n = Serial.write(p, min<size_t>(left, 512));
      p += n; left -= n;
      if (!n) delay(2);
    }
  } else if (c == 'P') {
    uint32_t t = millis();
    while (!Serial.available() && millis() - t < 200) delay(1);
    int p = Serial.read() - '0';
    if (p >= PAGE_MAIN && p <= PAGE_KB) { s_page = (Page)p; if (p == PAGE_SETUP) loadSetupFields(); s_dirty = true; }
  }
}

void loop() {
  serialDebug();
  static uint32_t lastTouch = 0;
  if (millis() - lastTouch >= 20) {
    lastTouch = millis();
    handleTouch();
  }

  // BOOT button: hold for the power menu; a short press closes it.
  if (Power::bootHeld() && s_page != PAGE_POWER) {
    s_pageBeforePower = s_page;
    s_page = PAGE_POWER;
    s_pressed = -1;
    s_dimmed = false;
    activity();
    s_dirty = true;
  }
  if (Power::bootPressed() && s_page == PAGE_POWER) { s_page = s_pageBeforePower; s_dirty = true; }

  String err = Sds200::takeError();
  if (err.length()) toast(err);

  // Scanner status changed: tell the call logger, count channel changes / signal as activity.
  const auto& st = Sds200::status();
  if (st.version != s_lastVersion) {
    static String lastChan;
    s_lastVersion = st.version;
    s_dirty = true;
    Calls::setContext(st.online, st.system, st.department, st.channel, st.tgid, st.freq);
    if (st.channel != lastChan && st.sig > 0) activity();
    lastChan = st.channel;
    if (!gsiAlert()) s_gsiAlertAck = false;
  }
  if (st.sig > 0 || Calls::inCall() || Calls::replaying()) activity();

  // Weather alert: wake, unmute, beep now and then while nothing else is playing.
  static bool alertWas = false;
  bool alert = alertShowing();
  if (alert) {
    activity();
    if (!alertWas) {
      if (AudioOut::muted()) AudioOut::setMuted(false);
      s_lastBeep = 0;
    }
    if (millis() - s_lastBeep > 4000 && !AudioOut::playing()) { s_lastBeep = millis(); AudioOut::beep(); }
    static uint32_t lastFlash = 0;
    if (millis() - lastFlash > 500) { lastFlash = millis(); s_dirty = true; }
  }
  if (alert != alertWas) s_dirty = true;
  alertWas = alert;

  // Dim when idle.
  uint16_t dimAfter = Settings::get().dimAfterS;
  bool shouldDim = dimAfter && millis() - s_lastActivity > dimAfter * 1000UL && s_page != PAGE_KB && s_page != PAGE_SETUP;
  if (shouldDim != s_dimmed) { s_dimmed = shouldDim; }
  applyBrightness();

  static uint32_t recentVer = 0;
  if (s_page == PAGE_RECENT && Calls::recentVersion() != recentVer) { recentVer = Calls::recentVersion(); s_dirty = true; }
  if (s_toast.length() && millis() >= s_toastUntil) { s_toast = ""; s_dirty = true; }
  if (s_rebootArmedUntil && millis() >= s_rebootArmedUntil) { s_rebootArmedUntil = 0; s_dirty = true; }

  // Refresh at least once a second (clock, audio indicator), and quickly on changes.
  uint32_t now = millis();
  if ((s_dirty && now - s_lastDraw > 40) || now - s_lastDraw > 1000) {
    s_dirty = false;
    s_lastDraw = now;
    redraw();
  }
}

}  // namespace Ui
