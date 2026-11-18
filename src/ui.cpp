#include "ui.h"
#include "config.h"
#include "display_st77922.h"
#include "touch_st77922.h"
#include "sds200.h"
#include "rtsp_audio.h"
#include "audio_out.h"
#include "battery.h"
#include <WiFi.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

namespace Ui {

static const int W = 480, H = 320;
static LGFX_Sprite s_canvas;

// Mapping from logical (landscape) to physical (portrait) pixels, measured at boot:
// phys = origin + lx * dx + ly * dy
static int s_ox, s_oy, s_dxx, s_dxy, s_dyx, s_dyy;

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
static const uint16_t C_HOLD = 0xFD20;    // orange

// ---------- buttons ----------
enum Page { PAGE_MAIN, PAGE_KEYS };
static Page s_page = PAGE_MAIN;

enum Action {
  A_NONE, A_KEY, A_VOL_DN, A_VOL_UP, A_SQL_DN, A_SQL_UP,
  A_SPK_DN, A_SPK_UP, A_SPK_MUTE, A_PAGE_KEYS, A_PAGE_MAIN,
  A_AUDIO_RECONNECT, A_REBOOT
};

struct Button {
  int x, y, w, h;
  const char* label;
  Action action;
  char key;        // for A_KEY
};

// Main page: 2 rows of 6 buttons along the bottom.
static const int BY1 = 204, BY2 = 264, BH = 54, BW = 76, BG = 4;
#define COL(i) (2 + (i) * (BW + BG))
static const Button kMain[] = {
  {COL(0), BY1, BW, BH, "SYSTEM", A_KEY, 'A'},
  {COL(1), BY1, BW, BH, "DEPT",   A_KEY, 'B'},
  {COL(2), BY1, BW, BH, "CHAN",   A_KEY, 'C'},
  {COL(3), BY1, BW, BH, "AVOID",  A_KEY, 'L'},
  {COL(4), BY1, BW, BH, "FUNC",   A_KEY, 'F'},
  {COL(5), BY1, BW, BH, "KEYS",   A_PAGE_KEYS, 0},
  {COL(0), BY2, BW, BH, "VOL -",  A_VOL_DN, 0},
  {COL(1), BY2, BW, BH, "VOL +",  A_VOL_UP, 0},
  {COL(2), BY2, BW, BH, "SQ -",   A_SQL_DN, 0},
  {COL(3), BY2, BW, BH, "SQ +",   A_SQL_UP, 0},
  {COL(4), BY2, BW, BH, "REPLAY", A_KEY, 'Y'},
  {COL(5), BY2, BW, BH, "MUTE",   A_SPK_MUTE, 0},
};

// Keys page: keypad on the left, navigation on the right.
static const int KW = 76, KH = 56, KG = 4, KY = 30;
#define KX(i) (2 + (i) * (KW + KG))
#define KYR(r) (KY + (r) * (KH + KG))
static const Button kKeys[] = {
  {KX(0), KYR(0), KW, KH, "1", A_KEY, '1'}, {KX(1), KYR(0), KW, KH, "2", A_KEY, '2'}, {KX(2), KYR(0), KW, KH, "3", A_KEY, '3'},
  {KX(0), KYR(1), KW, KH, "4", A_KEY, '4'}, {KX(1), KYR(1), KW, KH, "5", A_KEY, '5'}, {KX(2), KYR(1), KW, KH, "6", A_KEY, '6'},
  {KX(0), KYR(2), KW, KH, "7", A_KEY, '7'}, {KX(1), KYR(2), KW, KH, "8", A_KEY, '8'}, {KX(2), KYR(2), KW, KH, "9", A_KEY, '9'},
  {KX(0), KYR(3), KW, KH, ".", A_KEY, '.'}, {KX(1), KYR(3), KW, KH, "0", A_KEY, '0'}, {KX(2), KYR(3), KW, KH, "E/YES", A_KEY, 'E'},
  {KX(3), KYR(0), KW, KH, "MENU",  A_KEY, 'M'},
  {KX(4), KYR(0), KW, KH, "FUNC",  A_KEY, 'F'},
  {KX(5), KYR(0), KW, KH, "BACK",  A_PAGE_MAIN, 0},
  {KX(3), KYR(1), KW, KH, "<",     A_KEY, '<'},
  {KX(4), KYR(1), KW, KH, "PUSH",  A_KEY, '^'},
  {KX(5), KYR(1), KW, KH, ">",     A_KEY, '>'},
  {KX(3), KYR(2), KW, KH, "ZIP",   A_KEY, 'Z'},
  {KX(4), KYR(2), KW, KH, "RANGE", A_KEY, 'R'},
  {KX(5), KYR(2), KW, KH, "AVOID", A_KEY, 'L'},
  {KX(3), KYR(3), KW, KH, "SPK -", A_SPK_DN, 0},
  {KX(4), KYR(3), KW, KH, "SPK +", A_SPK_UP, 0},
  {KX(5), KYR(3), KW, KH, "MUTE",  A_SPK_MUTE, 0},
  // Bottom row: recovery tools
  {KX(0), KYR(4), 3 * KW + 2 * KG, 44, "RECONNECT AUDIO", A_AUDIO_RECONNECT, 0},
  {KX(3), KYR(4), 3 * KW + 2 * KG, 44, "REBOOT SCANNER",  A_REBOOT, 0},
};

// Reboot needs a second tap within this window.
static const uint32_t REBOOT_CONFIRM_MS = 4000;
static uint32_t s_rebootArmedUntil = 0;
static bool rebootArmed() { return s_rebootArmedUntil && millis() < s_rebootArmedUntil; }

static const Button* buttons(size_t& n) {
  if (s_page == PAGE_MAIN) { n = sizeof(kMain) / sizeof(kMain[0]); return kMain; }
  n = sizeof(kKeys) / sizeof(kKeys[0]);
  return kKeys;
}

static int s_pressed = -1;            // index of button under the finger
static uint32_t s_pressStart = 0;
static bool s_longSent = false;
static bool s_dirty = true;
static uint32_t s_lastVersion = 0;
static uint32_t s_lastDraw = 0;
static String s_toast;
static uint32_t s_toastUntil = 0;

static void toast(const String& t) { s_toast = t; s_toastUntil = millis() + 1200; s_dirty = true; }

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

static void drawButton(const Button& b, bool down, bool highlight) {
  bool armed = b.action == A_REBOOT && rebootArmed();
  s_canvas.fillRoundRect(b.x, b.y, b.w, b.h, 8, down ? C_BTN_DOWN : armed ? C_BAD : C_BTN);
  if (highlight) s_canvas.drawRoundRect(b.x, b.y, b.w, b.h, 8, C_HOLD);
  s_canvas.setFont(&fonts::Font2);
  s_canvas.setTextColor(C_TEXT);
  s_canvas.setTextDatum(textdatum_t::middle_center);
  s_canvas.drawString(armed ? "TAP AGAIN TO REBOOT" : b.label, b.x + b.w / 2, b.y + b.h / 2);
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

static void drawStatusBar() {
  const auto& st = Sds200::status();
  s_canvas.fillRect(0, 0, W, 24, C_PANEL);
  s_canvas.setFont(&fonts::Font2);

  // scanner link
  s_canvas.fillCircle(10, 12, 5, st.online ? C_OK : C_BAD);
  s_canvas.setTextColor(C_TEXT);
  String name = st.model.length() ? st.model : String("SDS200");
  s_canvas.drawString(name.c_str(), 20, 4);

  // audio link
  bool audio = AudioOut::playing();
  uint16_t ac = RtspAudio::connected() ? (audio ? C_OK : C_DIM) : C_BAD;
  s_canvas.fillCircle(100, 12, 5, ac);
  String a = AudioOut::muted() ? String("muted") : String(RtspAudio::status());

  // vol / squelch / speaker
  char buf[48];
  snprintf(buf, sizeof(buf), "VOL %s  SQ %s  SPK %d%%",
           st.vol >= 0 ? String(st.vol).c_str() : "-",
           st.sql >= 0 ? String(st.sql).c_str() : "-",
           AudioOut::volume());
  // audio status, cut short so it never runs into the levels text
  int levelsLeft = (W - 110) - s_canvas.textWidth(buf);
  fitText(a, 110, 4, levelsLeft - 8 - 110, &fonts::Font2, C_DIM);
  s_canvas.setTextColor(C_TEXT);
  s_canvas.setTextDatum(textdatum_t::top_right);
  s_canvas.drawString(buf, W - 110, 4);
  s_canvas.setTextDatum(textdatum_t::top_left);

  // battery (x 376..425, between the levels text and the signal bars)
  drawBattery(W - 104, 6);

  // signal bars (Sig 0-4)
  for (int i = 0; i < 4; i++) {
    int bh = 4 + i * 4;
    uint16_t c = (st.sig > i) ? C_OK : C_DIM;
    s_canvas.fillRect(W - 42 + i * 9, 20 - bh, 6, bh, c);
  }
}

static void drawMainInfo() {
  const auto& st = Sds200::status();
  int y = 30;

  if (!st.online) {
    s_canvas.setFont(&fonts::FreeSansBold12pt7b);
    s_canvas.setTextColor(C_BAD);
    s_canvas.drawString("Scanner not responding", 10, 60);
    s_canvas.setFont(&fonts::Font2);
    s_canvas.setTextColor(C_DIM);
    String l = String("Looking for ") + SCANNER_IP + " on UDP " + SCANNER_CTRL_PORT;
    s_canvas.drawString(l.c_str(), 10, 100);
    String w = String("Wi-Fi: ") + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("not connected"));
    s_canvas.drawString(w.c_str(), 10, 120);
    return;
  }

  // System
  fitText(st.system.length() ? st.system : st.mode, 10, y, 400, &fonts::FreeSans12pt7b, C_SYS);
  if (st.sysHold) { s_canvas.setFont(&fonts::Font2); s_canvas.setTextColor(C_HOLD); s_canvas.drawString("HOLD", 425, y + 4); }
  y += 32;
  // Department
  fitText(st.department, 10, y, 400, &fonts::FreeSans12pt7b, C_DEPT);
  if (st.deptHold) { s_canvas.setFont(&fonts::Font2); s_canvas.setTextColor(C_HOLD); s_canvas.drawString("HOLD", 425, y + 4); }
  y += 34;
  // Channel (big)
  fitText(st.channel.length() ? st.channel : String("Scanning..."), 10, y, 400, &fonts::FreeSansBold18pt7b,
          st.sig > 0 ? C_TEXT : C_DIM);
  if (st.chanHold) { s_canvas.setFont(&fonts::Font2); s_canvas.setTextColor(C_HOLD); s_canvas.drawString("HOLD", 425, y + 10); }
  y += 46;

  // Frequency / TGID line
  String line;
  if (st.tgid.length()) line += "TGID " + st.tgid + "   ";
  if (st.freq.length()) { String f = st.freq; f.trim(); line += f + "   "; }
  if (st.mod.length()) line += st.mod + "   ";
  if (st.service.length()) line += st.service;
  fitText(line, 10, y, 460, &fonts::Font4, C_TEXT);
  y += 30;

  // Site / scanner info line
  String sub;
  if (st.site.length()) sub += "Site: " + st.site + "   ";
  if (st.rssi.length()) sub += "RSSI " + st.rssi + "   ";
  if (st.info1.length()) sub += st.info1;
  fitText(sub, 10, y, 460, &fonts::Font2, C_DIM);

  // Scanner pop-up (e.g. "Quick Save?") as an overlay
  if (st.popup.length()) {
    s_canvas.fillRoundRect(60, 70, 360, 90, 10, C_PANEL);
    s_canvas.drawRoundRect(60, 70, 360, 90, 10, C_DEPT);
    s_canvas.setTextDatum(textdatum_t::middle_center);
    s_canvas.setFont(&fonts::FreeSans12pt7b);
    s_canvas.setTextColor(C_TEXT);
    s_canvas.drawString(st.popup.c_str(), 240, 105);
    s_canvas.setFont(&fonts::Font2);
    s_canvas.setTextColor(C_DIM);
    s_canvas.drawString("KEYS page: E/YES to accept", 240, 140);
    s_canvas.setTextDatum(textdatum_t::top_left);
  }
}

static void drawKeysHeader() {
  const auto& st = Sds200::status();
  s_canvas.fillRect(0, 0, W, 26, C_PANEL);
  String l = st.channel.length() ? st.channel : st.mode;
  fitText(l, 6, 5, 250, &fonts::Font2, C_TEXT);
  if (st.popup.length()) fitText(st.popup, 262, 5, 150, &fonts::Font2, C_DEPT);
  else {
    char buf[32];
    snprintf(buf, sizeof(buf), "SPK %d%%%s", AudioOut::volume(), AudioOut::muted() ? " (muted)" : "");
    fitText(buf, 282, 5, 130, &fonts::Font2, C_DIM);
  }
  drawBattery(W - 56, 7);   // top right, x 424..473
}

static void redraw() {
  s_canvas.fillScreen(C_BG);
  const auto& st = Sds200::status();
  size_t n;
  const Button* b = buttons(n);
  if (s_page == PAGE_MAIN) {
    drawStatusBar();
    drawMainInfo();
  } else {
    drawKeysHeader();
  }
  for (size_t i = 0; i < n; i++) {
    bool hl = false;
    if (b[i].action == A_KEY && b[i].key == 'A') hl = st.sysHold;
    if (b[i].action == A_KEY && b[i].key == 'B') hl = st.deptHold;
    if (b[i].action == A_KEY && b[i].key == 'C') hl = st.chanHold;
    if (b[i].action == A_SPK_MUTE) hl = AudioOut::muted();
    drawButton(b[i], (int)i == s_pressed, hl);
  }
  if (s_toast.length() && millis() < s_toastUntil) {
    s_canvas.fillRoundRect(90, 150, 300, 40, 8, C_BTN_DOWN);
    s_canvas.setTextDatum(textdatum_t::middle_center);
    s_canvas.setFont(&fonts::Font4);
    s_canvas.setTextColor(C_TEXT);
    s_canvas.drawString(s_toast.c_str(), 240, 170);
    s_canvas.setTextDatum(textdatum_t::top_left);
  }
  St77922::pushFrame((const uint16_t*)s_canvas.getBuffer());
}

// ---------- touch ----------
// Convert a physical (portrait) touch point to logical landscape coordinates.
static bool physToLogical(int px, int py, int& lx, int& ly) {
  // Solve phys = o + lx*dx + ly*dy for (lx, ly). dx/dy are unit axis vectors.
  int rx = px - s_ox, ry = py - s_oy;
  int det = s_dxx * s_dyy - s_dyx * s_dxy;
  if (det == 0) return false;
  lx = (rx * s_dyy - ry * s_dyx) / det;
  ly = (ry * s_dxx - rx * s_dxy) / det;
  return lx >= 0 && lx < W && ly >= 0 && ly < H;
}

static int hitTest(int x, int y) {
  size_t n;
  const Button* b = buttons(n);
  for (size_t i = 0; i < n; i++)
    if (x >= b[i].x && x < b[i].x + b[i].w && y >= b[i].y && y < b[i].y + b[i].h) return i;
  return -1;
}

static void doAction(const Button& b, bool longPress) {
  const auto& st = Sds200::status();
  switch (b.action) {
    case A_KEY: Sds200::key(b.key, longPress ? 'L' : 'P'); break;
    case A_VOL_DN: if (st.vol >= 0) Sds200::setVolume(st.vol - 1); break;
    case A_VOL_UP: if (st.vol >= 0) Sds200::setVolume(st.vol + 1); break;
    case A_SQL_DN: if (st.sql >= 0) Sds200::setSquelch(st.sql - 1); break;
    case A_SQL_UP: if (st.sql >= 0) Sds200::setSquelch(st.sql + 1); break;
    case A_SPK_DN: AudioOut::setVolume(AudioOut::volume() - 10); toast(String("Speaker ") + AudioOut::volume() + "%"); break;
    case A_SPK_UP: AudioOut::setVolume(AudioOut::volume() + 10); toast(String("Speaker ") + AudioOut::volume() + "%"); break;
    case A_SPK_MUTE: AudioOut::setMuted(!AudioOut::muted()); break;
    case A_PAGE_KEYS: s_page = PAGE_KEYS; break;
    case A_PAGE_MAIN: s_page = PAGE_MAIN; break;
    case A_AUDIO_RECONNECT: RtspAudio::reconnect(); toast("Reconnecting audio"); break;
    case A_REBOOT:
      if (rebootArmed()) {
        s_rebootArmedUntil = 0;
        Sds200::rebootScanner();
        RtspAudio::reconnect();   // audio reconnects by itself once the scanner is back
        toast("Rebooting scanner");
      } else {
        s_rebootArmedUntil = millis() + REBOOT_CONFIRM_MS;
      }
      break;
    default: break;
  }
  s_dirty = true;
}

static void handleTouch() {
  int16_t px, py;
  bool down = Touch::read(px, py);
  int lx = -1, ly = -1;
  if (down) {
    if (!physToLogical(px, py, lx, ly)) down = false;
#if TOUCH_DEBUG
    Serial.printf("touch raw=(%d,%d) screen=(%d,%d)\n", px, py, lx, ly);
#endif
  }

  if (down && s_pressed < 0) {
    int hit = hitTest(lx, ly);
    if (hit >= 0) { s_pressed = hit; s_pressStart = millis(); s_longSent = false; s_dirty = true; }
  } else if (down && s_pressed >= 0) {
    size_t n;
    const Button* b = buttons(n);
    // Long press (only meaningful for scanner keys)
    if (!s_longSent && b[s_pressed].action == A_KEY && millis() - s_pressStart > 700) {
      doAction(b[s_pressed], true);
      s_longSent = true;
      toast("Long press");
    }
  } else if (!down && s_pressed >= 0) {
    size_t n;
    const Button* b = buttons(n);
    int idx = s_pressed;
    s_pressed = -1;
    if (!s_longSent) doAction(b[idx], false);
    s_dirty = true;
  }
}

// ---------- setup ----------
static void measureRotation() {
  // Draw marker pixels at known logical points and find where they land in the
  // portrait buffer. This gives us the exact touch mapping for any rotation.
  auto find = [](uint16_t color, int& x, int& y) {
    const uint16_t* buf = (const uint16_t*)s_canvas.getBuffer();
    uint16_t c = (color >> 8) | (color << 8);   // buffer stores byte-swapped RGB565
    for (int i = 0; i < LCD_NATIVE_W * LCD_NATIVE_H; i++)
      if (buf[i] == c || buf[i] == color) { x = i % LCD_NATIVE_W; y = i / LCD_NATIVE_W; return; }
    x = y = 0;
  };
  s_canvas.fillScreen(0);
  s_canvas.drawPixel(0, 0, 0x1234);
  s_canvas.drawPixel(1, 0, 0x2345);
  s_canvas.drawPixel(0, 1, 0x3456);
  int ax, ay, bx, by, cx, cy;
  find(0x1234, ax, ay);
  find(0x2345, bx, by);
  find(0x3456, cx, cy);
  s_ox = ax; s_oy = ay;
  s_dxx = bx - ax; s_dxy = by - ay;
  s_dyx = cx - ax; s_dyy = cy - ay;
  log_i("rotation map: origin(%d,%d) dx(%d,%d) dy(%d,%d)", s_ox, s_oy, s_dxx, s_dxy, s_dyx, s_dyy);
}

bool begin() {
  s_canvas.setPsram(true);
  s_canvas.setColorDepth(16);
  if (!s_canvas.createSprite(LCD_NATIVE_W, LCD_NATIVE_H)) return false;
  s_canvas.setRotation(SCREEN_ROTATION);
  measureRotation();
  return true;
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
  St77922::pushFrame((const uint16_t*)s_canvas.getBuffer());
}

void loop() {
  static uint32_t lastTouch = 0;
  if (millis() - lastTouch >= 20) {
    lastTouch = millis();
    handleTouch();
  }

  uint32_t v = Sds200::status().version;
  if (v != s_lastVersion) { s_lastVersion = v; s_dirty = true; }
  if (s_toast.length() && millis() >= s_toastUntil) { s_toast = ""; s_dirty = true; }
  if (s_rebootArmedUntil && millis() >= s_rebootArmedUntil) { s_rebootArmedUntil = 0; s_dirty = true; }

  // Refresh at least once a second (audio indicator), and quickly on changes.
  uint32_t now = millis();
  if ((s_dirty && now - s_lastDraw > 40) || now - s_lastDraw > 1000) {
    s_dirty = false;
    s_lastDraw = now;
    redraw();
  }
}

}  // namespace Ui
