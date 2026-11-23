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
#include "version.h"
#include <WiFi.h>
#include <time.h>
#include <vector>
#include <algorithm>

namespace Ui {

// Logical screen: 480x320 in landscape, 320x480 in portrait (Tab5 held upright).
static int W = 480, H = 320;
static bool portrait() { return W < H; }

// Boot logo and animation, built into the firmware (see tools/make_boot_assets.sh).
extern const uint8_t kLogoStart[] asm("_binary_data_boot_logo_jpg_start");
extern const uint8_t kLogoEnd[]   asm("_binary_data_boot_logo_jpg_end");
extern const uint8_t kAnimStart[] asm("_binary_data_boot_anim_bin_start");
#if defined(BOARD_TAB5)
#include <driver/jpeg_decode.h>
// 640x360 versions for the Tab5 (shown doubled, straight to the screen)
extern const uint8_t kLogo5Start[] asm("_binary_data_tab5_boot_logo_jpg_start");
extern const uint8_t kLogo5End[]   asm("_binary_data_tab5_boot_logo_jpg_end");
extern const uint8_t kAnim5Start[] asm("_binary_data_tab5_boot_anim_bin_start");
static LGFX_Sprite* s_half = nullptr;
// The P4's hardware JPEG decoder writes straight into the half-size sprite's memory
// (the software decoder takes ~80 ms per 640x360 frame).
static jpeg_decoder_handle_t s_jpeg = nullptr;
static uint8_t* s_jpegIn = nullptr;
static size_t s_jpegInSize = 0, s_halfBufSize = 0;
static uint8_t* s_halfBuf = nullptr;

static LGFX_Sprite& halfSprite() {
  if (!s_half) {
    s_half = new LGFX_Sprite(&Board::canvas());
    jpeg_decode_memory_alloc_cfg_t outCfg = {JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    s_halfBuf = (uint8_t*)jpeg_alloc_decoder_mem(640 * 368 * 2, &outCfg, &s_halfBufSize);   // rows padded to 16
    if (s_halfBuf) {
      s_half->setBuffer(s_halfBuf, 640, 360, Board::canvas().getColorDepth());
      jpeg_decode_engine_cfg_t ec = {0, 200};
      jpeg_decode_memory_alloc_cfg_t inCfg = {JPEG_DEC_ALLOC_INPUT_BUFFER};
      if (jpeg_new_decoder_engine(&ec, &s_jpeg) == ESP_OK)
        s_jpegIn = (uint8_t*)jpeg_alloc_decoder_mem(64 * 1024, &inCfg, &s_jpegInSize);
    } else {
      s_half->setPsram(true);
      s_half->setColorDepth(Board::canvas().getColorDepth());
      s_half->createSprite(640, 360);
    }
  }
  return *s_half;
}

// Decode a 640x360 JPEG into the half-size sprite (hardware if possible).
static bool halfJpeg(const uint8_t* d, uint32_t len) {
  LGFX_Sprite& h = halfSprite();
  if (s_jpeg && s_jpegIn && len <= s_jpegInSize) {
    memcpy(s_jpegIn, d, len);
    jpeg_decode_cfg_t cfg = {JPEG_DECODE_OUT_FORMAT_RGB565, JPEG_DEC_RGB_ELEMENT_ORDER_BGR, JPEG_YUV_RGB_CONV_STD_BT601};
    uint32_t out = 0;
    if (jpeg_decoder_process(s_jpeg, &cfg, s_jpegIn, len, s_halfBuf, s_halfBufSize, &out) == ESP_OK) return true;
  }
  return h.drawJpg(d, len, 0, 0);
}

// The half-size sprite shares its memory with the hardware JPEG decoder, which (like the
// screen) stores RGB565 little-endian, while the sprite itself writes byte-swapped
// RGB565. Text colours are therefore given byte-swapped so they come out right.
static uint16_t halfColor(LGFX_Sprite& h, uint16_t c) {
  return h.getColorDepth() == lgfx::rgb565_2Byte ? (uint16_t)((c >> 8) | (c << 8)) : c;
}

static void halfStatus(const char* status) {
  LGFX_Sprite& h = halfSprite();
  h.setFont(&fonts::Font0);                       // version in the bottom-right corner
  h.setTextDatum(textdatum_t::bottom_right);
  h.setTextColor(halfColor(h, 0x8410));
  h.drawString("v" UC_VERSION, 636, 357);
  if (!status || !*status) return;
  h.setFont(&fonts::DejaVu18);
  h.setTextDatum(textdatum_t::bottom_center);
  h.setTextColor(halfColor(h, 0x8410));
  h.drawString(status, 320, 355);
}
#endif
// The UI is laid out on a logical 480x320 screen. Pen draws it onto the board's
// canvas, scaling positions and swapping in larger fonts on bigger screens
// (scale 1 on the FNK0104N; 2.67 x 2.25 on the Tab5's 1280x720).
struct Pen {
  LGFX_Sprite* c = nullptr;
  float kx = 1, ky = 1;
  int ox = 0, oy = 0;                            // origin of the band / image being drawn (canvas pixels)
  int X(int x) const { return (int)lroundf(x * kx) - ox; }
  int Y(int y) const { return (int)lroundf(y * ky) - oy; }
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

  // When drawing in bands, skip anything that lies entirely outside the current band.
  // Skip anything outside the band / image being drawn or outside the clip region.
  bool clipping = false;
  int cx0 = 0, cy0 = 0, cx1 = 0, cy1 = 0;        // clip region (canvas pixels)
  bool off(int x0, int x1, int y0 = -100000, int y1 = 100000) const {
    if (clipping) return X(x1) <= cx0 || X(x0) >= cx1 || (y0 > -100000 && (Y(y1) <= cy0 || Y(y0) >= cy1));
    return ox && (X(x1) <= 0 || X(x0) >= c->width());
  }
  void fillScreen(uint16_t col) { c->fillScreen(col); }
  void fillRect(int x, int y, int w, int h, uint16_t col) { if (off(x, x + w, y, y + h)) return; c->fillRect(X(x), Y(y), X(x + w) - X(x), Y(y + h) - Y(y), col); }
  void drawRect(int x, int y, int w, int h, uint16_t col) { if (off(x, x + w, y, y + h)) return; c->drawRect(X(x), Y(y), X(x + w) - X(x), Y(y + h) - Y(y), col); }
  void fillRoundRect(int x, int y, int w, int h, int r, uint16_t col) { if (off(x, x + w, y, y + h)) return; c->fillRoundRect(X(x), Y(y), X(x + w) - X(x), Y(y + h) - Y(y), R(r), col); }
  void drawRoundRect(int x, int y, int w, int h, int r, uint16_t col) { if (off(x, x + w, y, y + h)) return; c->drawRoundRect(X(x), Y(y), X(x + w) - X(x), Y(y + h) - Y(y), R(r), col); }
  void drawFastHLine(int x, int y, int w, uint16_t col) { if (off(x, x + w, y, y + 1)) return; c->fillRect(X(x), Y(y), X(x + w) - X(x), Y(y + 1) - Y(y), col); }
  void fillArc(int x, int y, int r0, int r1, float a0, float a1, uint16_t col) {
    if (off(x - r1, x + r1, y - r1, y + r1)) return;
    c->fillArc(X(x), Y(y), R(r0), R(r1), a0, a1, col);
  }
  void fillCircle(int x, int y, int r, uint16_t col) { if (off(x - r, x + r, y - r, y + r)) return; c->fillCircle(X(x), Y(y), R(r), col); }
  void fillTriangle(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t col) {
    if (off(min(x0, min(x1, x2)), max(x0, max(x1, x2)), min(y0, min(y1, y2)), max(y0, max(y1, y2)))) return;
    c->fillTriangle(X(x0), Y(y0), X(x1), Y(y1), X(x2), Y(y2), col);
  }
  void setFont(const lgfx::IFont* f) { c->setFont(mapFont(f)); }
  void setTextColor(uint16_t col) { c->setTextColor(col); }
  void setTextDatum(lgfx::textdatum_t d) { c->setTextDatum(d); }
  void drawString(const char* t, int x, int y) {
    if (ox || clipping) {                        // skip text that misses this band / region
      int w = c->textWidth(t), px = X(x), py = Y(y), fh = c->fontHeight();
      int d = c->getTextDatum() & 3;             // 0 left, 1 centre, 2 right
      int x0 = d == 0 ? px : d == 1 ? px - w / 2 : px - w;
      int lx0 = clipping ? cx0 : 0, lx1 = clipping ? cx1 : c->width();
      if (x0 + w <= lx0 || x0 >= lx1) return;
      if (clipping && (py + fh <= cy0 || py - fh >= cy1)) return;
    }
    c->drawString(t, X(x), Y(y));
  }
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
// Shortened text is remembered for one redraw, so drawing in bands doesn't redo the
// (slow) width measuring for every band.
struct FitCache { String in; int maxW; const lgfx::IFont* font; String out; };
static std::vector<FitCache> s_fitCache;

static void fitText(const String& txt, int x, int y, int maxW, const lgfx::IFont* font, uint16_t color) {
  s_canvas.setFont(font);
  s_canvas.setTextColor(color);
  for (auto& f : s_fitCache)
    if (f.maxW == maxW && f.font == font && f.in == txt) { s_canvas.drawString(f.out.c_str(), x, y); return; }
  String t = txt;
  if (s_canvas.textWidth(t.c_str()) > maxW) {
    while (t.length() > 1 && s_canvas.textWidth((t + "...").c_str()) > maxW) t.remove(t.length() - 1);
    t += "...";
  }
  s_fitCache.push_back({txt, maxW, font, t});
  s_canvas.drawString(t.c_str(), x, y);
}

static void centerText(const String& t, int x, int y, const lgfx::IFont* font, uint16_t color) {
  s_canvas.setFont(font);
  s_canvas.setTextColor(color);
  s_canvas.setTextDatum(textdatum_t::middle_center);
  s_canvas.drawString(t.c_str(), x, y);
  s_canvas.setTextDatum(textdatum_t::top_left);
}

// Like fitText, but a name too long for one line breaks at a space onto a second line.
// Returns the number of lines used.
static int wrapText(const String& txt, int x, int y, int maxW, int lineH, const lgfx::IFont* font, uint16_t color) {
  s_canvas.setFont(font);
  if (s_canvas.textWidth(txt.c_str()) > maxW) {
    int cut = -1;
    for (int i = txt.indexOf(' '); i > 0; i = txt.indexOf(' ', i + 1)) {
      if (s_canvas.textWidth(txt.substring(0, i).c_str()) > maxW) break;
      cut = i;
    }
    if (cut > 0) {
      fitText(txt.substring(0, cut), x, y, maxW, font, color);
      fitText(txt.substring(cut + 1), x, y + lineH, maxW, font, color);
      return 2;
    }
  }
  fitText(txt, x, y, maxW, font, color);
  return 1;
}

// Scanner-style key: raised charcoal body with a bevel, backlit label with a halo,
// a faint light leak around the edge. Pressed = pushed in and lit up. Active = LED bar.
static const uint16_t K_RIM = 0x0841, K_BODY = 0x2104, K_TOP = 0x2945, K_HI = 0x4208, K_SH = 0x0000;

static void drawButtonNow(const Button& b, bool down);

// On big screens a backlit key is slow to draw (its label is drawn five times with
// smooth fonts), so each key's look is kept as a small image and reused until its
// label, state or colour changes.
struct KeyImage { String key; LGFX_Sprite* img; };
static std::vector<KeyImage> s_keyCache;

static void drawButton(const Button& b, bool down) {
  bool custom = b.action == A_RECENT_ROW || b.action == A_WIFI_PICK || b.action == A_FAV || b.action == A_SETUP_FIELD;
  if (!s_canvas.scaled() || s_canvas.ox || s_canvas.oy) { drawButtonNow(b, down); return; }
  bool armed = b.action == A_REBOOT && rebootArmed();
  String key = String(b.x) + "," + b.y + "," + b.w + "," + b.h + "|" + b.label + "|" + (int)down + (int)b.hl + (int)armed +
               "|" + b.bg + "|" + (int)b.glow + "|" + Settings::keyColor() + (custom ? "|c" : "");
  LGFX_Sprite* img = nullptr;
  for (auto& k : s_keyCache) if (k.key == key) { img = k.img; break; }
  int px = s_canvas.X(b.x), py = s_canvas.Y(b.y);
  if (!img) {
    if (s_keyCache.size() >= 96) {               // keep memory bounded
      for (auto& k : s_keyCache) delete k.img;
      s_keyCache.clear();
    }
    int w = s_canvas.X(b.x + b.w) - px, h = s_canvas.Y(b.y + b.h) - py;
    img = new LGFX_Sprite(s_canvas.c);
    img->setPsram(true);
    img->setColorDepth(s_canvas.c->getColorDepth());
    if (!img->createSprite(w, h)) { delete img; drawButtonNow(b, down); return; }
    img->fillScreen(C_BG);
    LGFX_Sprite* main = s_canvas.c;
    s_canvas.c = img;
    s_canvas.ox = px;
    s_canvas.oy = py;
    drawButtonNow(b, down);
    s_canvas.c = main;
    s_canvas.ox = s_canvas.oy = 0;
    s_keyCache.push_back({key, img});
  }
  img->pushSprite(s_canvas.c, px, py);
}

static void drawButtonNow(const Button& b, bool down) {
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
  if (s_canvas.textWidth(label.c_str()) > w - (label.length() == 1 ? 2 : 8)) s_canvas.setFont(&fonts::Font0);   // small keys
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
  int ax = max(100, 20 + s_canvas.textWidth(left.c_str()) + 10);   // audio dot after the clock

  // audio link
  bool audio = AudioOut::playing();
  uint16_t ac = RtspAudio::connected() ? (audio ? C_OK : C_DIM) : C_BAD;
  s_canvas.fillCircle(ax, 12, 5, ac);
  String a = Calls::replaying() ? String("replay") : AudioOut::muted() ? String("muted") : String(RtspAudio::status());

  // board speaker volume (right-aligned), REC to its left while recording
  // (on big screens the SPK level uses a smaller font so it sits clear of REC)
  const lgfx::IFont* spkFont = s_canvas.scaled() ? (const lgfx::IFont*)&fonts::Font0 : (const lgfx::IFont*)&fonts::Font2;
  int spkY = s_canvas.scaled() ? 8 : 4;
  char buf[24];
  snprintf(buf, sizeof(buf), "SPK %d%%", AudioOut::volume());
  // Wi-Fi icon: arcs lit by signal strength (red dot when not connected)
  {
    int wx = W - 117, wy = 19;
    bool up = WiFi.status() == WL_CONNECTED;
    int rssi = up ? WiFi.RSSI() : -100;
    int bars = !up ? 0 : rssi > -60 ? 3 : rssi > -72 ? 2 : 1;
    for (int i = 0; i < 3; i++)
      s_canvas.fillArc(wx, wy, 4 + i * 4, 6 + i * 4, 225, 315, i < bars ? C_TEXT : C_DIM);
    s_canvas.fillCircle(wx, wy, 2, up ? C_TEXT : C_BAD);
  }
  int right = W - 128;
  s_canvas.setFont(spkFont);
  // (portrait: no room for the speaker level here; the main page shows it instead)
  int levelsLeft = portrait() ? right + 2 : right - s_canvas.textWidth(buf);
  int textEnd = levelsLeft - 8;
  if (Calls::recordingNow()) {
    s_canvas.setFont(&fonts::Font2);
    int recW = s_canvas.textWidth("REC");
    int rx = levelsLeft - 8 - recW - 13;           // dot + gap + "REC", then a gap before SPK
    s_canvas.fillCircle(rx + 5, 12, 5, C_BAD);
    s_canvas.setTextColor(C_BAD);
    s_canvas.drawString("REC", rx + 13, 4);
    textEnd = rx - 6;
  }
  if (textEnd - (ax + 10) > 20) fitText(a, ax + 10, 4, textEnd - (ax + 10), &fonts::Font2, C_DIM);
  if (!portrait()) {
    s_canvas.setFont(spkFont);
    s_canvas.setTextColor(C_TEXT);
    s_canvas.setTextDatum(textdatum_t::top_right);
    s_canvas.drawString(buf, right, spkY);
    s_canvas.setTextDatum(textdatum_t::top_left);
  }

  drawBattery(W - 104, 6);

  // signal bars (Sig 0-4)
  for (int i = 0; i < 4; i++) {
    int bh = 4 + i * 4;
    uint16_t c = (st.sig > i) ? C_OK : C_DIM;
    s_canvas.fillRect(W - 42 + i * 9, 20 - bh, 6, bh, c);
  }
}

// Main page: one row along the bottom plus SCAN / WX above MUTE / KEYS.
// (landscape: MBY 264, MBW 116; set for the current orientation by setLayout())
static const int MBH = 54, MBG = 4;
static int MBY = 264, MBW = 116;
#define MCOL(i) (2 + (i) * (MBW + MBG))
static int WX_TOP = MBY - MBH - MBG, WX_LEFT = MCOL(2);

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
  } else if (portrait()) {
    // Tall screen: names may take two lines, and each detail gets a line of its own.
    int nameW = W - 70;
    int n = wrapText(st.system.length() ? st.system : st.mode, 10, y, nameW, 23, &fonts::FreeSans12pt7b, C_SYS);
    if (st.sysHold) { s_canvas.setFont(&fonts::Font2); s_canvas.setTextColor(C_HOLD); s_canvas.drawString("HOLD", W - 52, y + 4); }
    y += 23 * n + 8;
    n = wrapText(st.department, 10, y, nameW, 23, &fonts::FreeSans12pt7b, C_DEPT);
    if (st.deptHold) { s_canvas.setFont(&fonts::Font2); s_canvas.setTextColor(C_HOLD); s_canvas.drawString("HOLD", W - 52, y + 4); }
    y += 23 * n + 8;
    n = wrapText(st.channel.length() ? st.channel : String("Scanning..."), 10, y, nameW, 30, &fonts::FreeSansBold18pt7b,
                 st.sig > 0 ? C_TEXT : C_DIM);
    if (st.chanHold) { s_canvas.setFont(&fonts::Font2); s_canvas.setTextColor(C_HOLD); s_canvas.drawString("HOLD", W - 52, y + 8); }
    y += 30 * n + 12;

    int bottom = WX_TOP - 4;
    auto line = [&](const String& t, const lgfx::IFont* f, uint16_t col, int h) {
      if (!t.length() || y + h > bottom) return;
      fitText(t, 10, y, W - 20, f, col);
      y += h + 5;
    };
    String tg = st.tgid; tg.trim();
    if (tg.length() && tg.indexOf("None") < 0 && tg.indexOf("---") < 0) line(tg, &fonts::Font4, C_TEXT, 21);
    String f = st.freq; f.trim();
    line(f.length() && st.mod.length() ? f + "   " + st.mod : f + st.mod, &fonts::Font4, C_TEXT, 21);
    line(st.service, &fonts::Font4, C_DIM, 21);
    if (st.site.length()) line("Site: " + st.site, &fonts::Font2, C_DIM, 15);
    String rssi = st.rssi; rssi.trim();
    String sub;
    if (rssi.length() && rssi != "-999") sub += "RSSI " + rssi + "   ";
    sub += st.info1;
    line(sub, &fonts::Font2, C_DIM, 15);
    line(st.info2, &fonts::Font2, C_DIM, 15);
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
    add(MCOL(0), WX_TOP, 2 * MBW + MBG, MBH, rebootArmed() ? "TAP AGAIN TO REBOOT"
                                              : portrait() ? "STUCK: REBOOT SCANNER" : "AUDIO STUCK: REBOOT SCANNER",
        A_REBOOT, 0, false, C_DARKRED);
  add(MCOL(3), WX_TOP, MBW, MBH, "WX", A_WX, 0, st.online && Sds200::inWeather());
  if (portrait() && !(RtspAudio::lockedOut() && st.online))   // the speaker level (no room in the top bar)
    centerText(AudioOut::muted() ? String("Speaker muted") : "Speaker " + String(AudioOut::volume()) + "%",
               MCOL(1) - MBG / 2, WX_TOP + MBH / 2, &fonts::Font2, AudioOut::muted() ? C_HOLD : C_DIM);

  if (st.popup.length()) {
    int pw = min(360, W - 20), px = (W - pw) / 2;
    s_canvas.fillRoundRect(px, 70, pw, 90, 10, C_PANEL);
    s_canvas.drawRoundRect(px, 70, pw, 90, 10, C_DEPT);
    centerText(st.popup, W / 2, 105, &fonts::FreeSans12pt7b, C_TEXT);
    centerText("KEYS page: E/YES to accept", W / 2, 140, &fonts::Font2, C_DIM);
  }
}

// ---------- KEYS ----------
static const int KW = 76, KH = 55, KG = 3, KY = 28;
#define KX(i) (2 + (i) * (KW + KG + 1))
#define KYR(r) (KY + (r) * (KH + KG))

static void header(const String& title, const String& right = "") {
  s_canvas.fillRect(0, 0, W, 26, C_PANEL);
  if (portrait()) {
    fitText(title, 6, 5, right.length() ? 112 : W - 70, &fonts::Font2, C_TEXT);
    if (right.length()) fitText(right, 124, 5, W - 186, &fonts::Font2, C_DIM);
  } else {
    fitText(title, 6, 5, right.length() ? 250 : 400, &fonts::Font2, C_TEXT);
    if (right.length()) fitText(right, 262, 5, 150, &fonts::Font2, C_DIM);
  }
  drawBattery(W - 56, 7);
}

static void pageKeys() {
  const auto& st = Sds200::status();
  char buf[32];
  snprintf(buf, sizeof(buf), "SPK %d%%%s", AudioOut::volume(), AudioOut::muted() ? " (muted)" : "");
  if (st.popup.length()) header(st.channel.length() ? st.channel : st.mode, st.popup);
  else header(st.channel.length() ? st.channel : st.mode, buf);

  static const char* pad = "123456789.0E";
  struct { const char* l; char k; } right[] = {
    {"SYSTEM", 'A'}, {"DEPT", 'B'}, {"CHAN", 'C'}, {"<", '<'}, {"PUSH", '^'}, {">", '>'},
    {"MENU", 'M'}, {"FUNC", 'F'}, {"AVOID", 'L'}, {"ZIP", 'Z'}, {"RANGE", 'R'}, {"REPLAY", 'Y'},
  };
  bool rec = Settings::get().record;
  if (portrait()) {
    // Three columns: scanner function keys, then the number pad, then this app's keys.
    const int g = 3, kw = (W - 4 - 2 * g) / 3, gap = 6;
    const int kh = (H - 30 - 9 * g - 2 * gap) / 10;
    auto px = [&](int c) { return 2 + c * (kw + g); };
    auto py = [&](int r) { return 30 + r * (kh + g) + (r >= 4 ? gap : 0) + (r >= 8 ? gap : 0); };
    for (int i = 0; i < 12; i++) {
      bool hl = (right[i].k == 'A' && st.sysHold) || (right[i].k == 'B' && st.deptHold) || (right[i].k == 'C' && st.chanHold);
      add(px(i % 3), py(i / 3), kw, kh, right[i].l, A_KEY, right[i].k, hl);
    }
    for (int i = 0; i < 12; i++) {
      char k = pad[i];
      add(px(i % 3), py(4 + i / 3), kw, kh, k == 'E' ? String("E/YES") : String(k), A_KEY, k);
    }
    add(px(0), py(8), kw, kh, rec ? "REC ON" : "REC OFF", A_REC, 0, rec, rec ? C_DARKRED : C_BTN);
    add(px(1), py(8), kw, kh, "RECENT", A_GOTO, PAGE_RECENT);
    add(px(2), py(8), kw, kh, "FAVS", A_GOTO, PAGE_FAVS);
    add(px(0), py(9), kw, kh, "SETTINGS", A_GOTO, PAGE_SETTINGS);
    add(px(1), py(9), kw, kh, "POWER", A_POWER_MENU);
    add(px(2), py(9), kw, kh, "BACK", A_GOTO, PAGE_MAIN);
    return;
  }
  for (int i = 0; i < 12; i++) {
    char k = pad[i];
    add(KX(i % 3), KYR(i / 3), KW, KH, k == 'E' ? String("E/YES") : String(k), A_KEY, k);
  }
  for (int i = 0; i < 12; i++) {
    bool hl = (right[i].k == 'A' && st.sysHold) || (right[i].k == 'B' && st.deptHold) || (right[i].k == 'C' && st.chanHold);
    add(KX(3 + i % 3), KYR(i / 3), KW, KH, right[i].l, A_KEY, right[i].k, hl);
  }
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
  const char* how = Board::hasBootButton() ? "press BOOT" : "press the power button";
  if (portrait()) {
    centerText(Board::hasBootButton() ? "After POWER OFF, press" : "After POWER OFF, press the", W / 2, 85, &fonts::Font2, C_DIM);
    centerText(Board::hasBootButton() ? "BOOT to turn it back on" : "power button to turn it back on", W / 2, 103, &fonts::Font2, C_DIM);
    int bw = (W - 20) / 2;
    add(8, 130, bw, 120, "POWER OFF", A_POWER_OFF, 0, false, C_BAD);
    add(12 + bw, 130, bw, 120, "RESTART", A_RESTART);
    if (Board::hasLauncher()) {
      add(8, 258, bw, 120, "LAUNCHER", A_LAUNCHER);
      add(12 + bw, 258, bw, 120, "CANCEL", A_POWER_CANCEL);
    } else {
      add(8, 258, W - 16, 120, "CANCEL", A_POWER_CANCEL);
    }
    return;
  }
  centerText(String("After POWER OFF, ") + how + " to turn it back on", W / 2, 85, &fonts::Font2, C_DIM);
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
  header("Settings", "Version " UC_VERSION);
  const auto& c = Settings::get();
  struct { const char* label; String value; } rows[] = {
    {"Brightness", String(c.brightness) + "%"},
    {"Dim after", dimAfterText(c.dimAfterS)},
    {portrait() ? "Dim level" : "Dimmed level", c.dimLevel ? String(c.dimLevel) + "%" : String("Off")},
  };
  bool tall = portrait();
  for (int r = 0; r < 3; r++) {
    int y = 30 + r * 46;
    if (tall) {
      fitText(rows[r].label, 10, y + 13, 118, &fonts::Font2, C_TEXT);
      add(130, y, 50, 42, "-", A_ADJ, r * 2);
      centerText(rows[r].value, (180 + W - 52) / 2, y + 21, &fonts::Font4, C_DEPT);
      add(W - 52, y, 50, 42, "+", A_ADJ, r * 2 + 1);
    } else {
      fitText(rows[r].label, 12, y + 10, 220, &fonts::Font4, C_TEXT);
      add(250, y, 60, 42, "-", A_ADJ, r * 2);
      centerText(rows[r].value, 355, y + 21, &fonts::Font4, C_DEPT);
      add(400, y, 60, 42, "+", A_ADJ, r * 2 + 1);
    }
  }

  // Key light: sample keys, tap one to use it
  fitText("Key light", 12, 170, 200, &fonts::Font2, C_TEXT);
  s_canvas.setFont(&fonts::Font2);
  int kx = max(90, 12 + s_canvas.textWidth("Key light") + 10);
  fitText(kKeyLights[Settings::keyColor()].name, kx, 170, 200, &fonts::Font2, s_glow[Settings::keyColor()].full);
  for (int i = 0; i < NUM_KEYLIGHTS; i++) {
    if (tall) add(2 + (i % 5) * 63, 188 + (i / 5) * 44, 60, 40, kKeyLights[i].name, A_KEYLIGHT, i, i == Settings::keyColor(), C_BTN, i);
    else add(2 + i * 48, 188, 44, 40, kKeyLights[i].name, A_KEYLIGHT, i, i == Settings::keyColor(), C_BTN, i);
  }

  String wifi = WiFi.status() == WL_CONNECTED ? "Wi-Fi: " + WiFi.SSID() + "  IP " + WiFi.localIP().toString()
                                              : String("Wi-Fi: not connected");
  String sd = Calls::sdInfo() + (Calls::sdOk() ? (Settings::get().record ? ", recording on" : ", recording off") : String(""));
  String upd = Board::hasLauncher() ? String("via Launcher") : Ota::enabled() ? String(OTA_HOSTNAME) + ".local" : String("off");
  if (tall) {
    int y = 280;
    String lines[4] = {wifi, "Scanner: " + Settings::scannerIp(), sd, "Updates: " + upd};
    for (const String& l : lines) {
      fitText(l, 12, y, W - 24, &fonts::Font0, C_DIM);
      y += 14;
    }
    int bw = (W - 6) / 2;
    add(2, H - 116, bw, 54, "WI-FI SETUP", A_GOTO, PAGE_SETUP);
    add(4 + bw, H - 116, bw, 54, "RECONNECT AUDIO", A_AUDIO_RECONNECT);
    add(2, H - 58, bw, 54, "REBOOT SCANNER", A_REBOOT);
    add(4 + bw, H - 58, bw, 54, "BACK", A_GOTO, PAGE_KEYS);
    return;
  }
  int y = 232;
  fitText(wifi + "   Scanner: " + Settings::scannerIp(), 12, y, 456, &fonts::Font0, C_DIM); y += 12;
  fitText(sd + "   Updates: " + upd, 12, y, 456, &fonts::Font0, C_DIM);

  add(2, 262, 116, 54, "WI-FI SETUP", A_GOTO, PAGE_SETUP);
  add(122, 262, 138, 54, "RECONNECT AUDIO", A_AUDIO_RECONNECT);
  add(264, 262, 134, 54, "REBOOT SCANNER", A_REBOOT);
  add(402, 262, 76, 54, "BACK", A_GOTO, PAGE_KEYS);
}

// ---------- RECENT ----------
static const int ROW_H = 44, ROW_Y = 30;
static int ROWS = 5;                    // list rows on screen (set by setLayout())

// A row of n equal buttons across the bottom of the screen (or at y).
static int bottomW(int n) { return (W - 4 - (n - 1) * 4) / n; }
static int bottomX(int n, int i) { return 2 + i * (bottomW(n) + 4); }

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
    add(2, y, W - 4, ROW_H, "", A_RECENT_ROW, i, i == s_playingRow);
    drawButton(s_btns.back(), false);
    char t[16] = "";
    if (!clockText(t, sizeof(t), c.start)) snprintf(t, sizeof(t), "--:--");
    int tx = portrait() ? 86 : 78;                 // (the time is wider on the narrower scale)
    fitText(t, 8, y + 4, tx - 8, &fonts::Font2, C_DIM);
    fitText(c.channel.length() ? c.channel : c.tgid, tx, y + 4, W - 82 - tx, &fonts::Font2, C_TEXT);
    fitText(c.dept.length() ? c.dept : c.system, tx, y + 24, W - 82 - tx, &fonts::Font0, C_DIM);
    char d[16];
    snprintf(d, sizeof(d), "%lus", (unsigned long)((c.durMs + 500) / 1000));
    fitText(d, W - 76, y + 4, 40, &fonts::Font2, C_DIM);
    if (c.wav.length()) {
      if (i == s_playingRow) s_canvas.fillRect(W - 30, y + 14, 14, 14, C_OK);
      else s_canvas.fillTriangle(W - 30, y + 12, W - 30, y + 32, W - 14, y + 22, C_OK);
    }
  }
  add(bottomX(4, 0), H - 56, bottomW(4), 54, "UP", A_LIST_UP);
  add(bottomX(4, 1), H - 56, bottomW(4), 54, "DOWN", A_LIST_DN);
  add(bottomX(4, 2), H - 56, bottomW(4), 54, "STOP", A_STOP_PLAY, 0, Calls::replaying());
  add(bottomX(4, 3), H - 56, bottomW(4), 54, "BACK", A_GOTO, PAGE_KEYS);
}

// ---------- FAVS ----------
static void pageFavs() {
  if (portrait()) header("Favorites", "hold = save");
  else header("Favorites: tap = hold there, hold 1 s = save current");
  const auto& c = Settings::get();
  bool tall = portrait();
  for (int i = 0; i < Settings::NUM_FAVS; i++) {
    int x = tall ? 2 : 2 + (i % 2) * 240, y = tall ? 30 + i * 64 : 30 + (i / 2) * 76;
    int w = tall ? W - 4 : 236, h = tall ? 60 : 72, tw = w - 20, y2 = tall ? 34 : 40;
    add(x, y, w, h, "", A_FAV, i);
    drawButton(s_btns.back(), false);
    const auto& f = c.favs[i];
    if (f.used) {
      fitText(strlen(f.dept) ? f.dept : f.sys, x + 10, y + 10, tw, &fonts::Font2, C_TEXT);
      fitText(strlen(f.dept) ? f.sys : "(whole system)", x + 10, y + y2, tw, &fonts::Font2, C_DIM);
    } else {
      fitText(String("Slot ") + (i + 1) + " - empty", x + 10, y + 10, tw, &fonts::Font2, C_DIM);
      fitText("hold to save current", x + 10, y + y2, tw, &fonts::Font2, C_DIM);
    }
  }
  add(bottomX(2, 0), H - 56, bottomW(2), 54, tall ? "SCAN" : "SCAN (release hold)", A_SCAN);
  add(bottomX(2, 1), H - 56, bottomW(2), 54, "BACK", A_GOTO, PAGE_KEYS);
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
  bool tall = portrait();
  for (int r = 0; r < 4; r++) {
    int y = tall ? 32 + r * 70 : 32 + r * 48;
    int fx = tall ? 2 : 160, fy = tall ? y + 20 : y;   // portrait: label above its field
    fitText(rows[r].label, 10, tall ? y : y + 13, 150, &fonts::Font2, C_TEXT);
    add(fx, fy, W - 2 - fx, 44, "", A_SETUP_FIELD, r);
    drawButton(s_btns.back(), false);
    fitText(rows[r].value, fx + 12, fy + 13, W - 24 - fx, &fonts::Font2, C_DEPT);
  }
  int my = tall ? 316 : 232;
  uint16_t mc = s_setupMsg.startsWith("Connected") || s_setupMsg.startsWith("Saved") ? C_OK : C_HOLD;
  String msg = s_setupMsg;
  if (!msg.length()) {
    mc = C_DIM;
    msg = WiFi.status() == WL_CONNECTED
              ? "This board: " + WiFi.localIP().toString() + " on \"" + WiFi.SSID() + "\"  (" + String(WiFi.RSSI()) + " dBm)"
              : String("This board: not connected to Wi-Fi");
  }
  if (tall) wrapText(msg, 10, my, W - 20, 20, &fonts::Font2, mc);
  else fitText(msg, 10, my, 460, &fonts::Font2, mc);
  if (tall) {
    add(2, H - 116, W - 4, 54, "SAVE & CONNECT", A_SAVE_CONNECT, 0, false, C_GREENKEY);
    add(bottomX(2, 0), H - 58, bottomW(2), 54, "FORGET WI-FI", A_FORGET_WIFI);
    add(bottomX(2, 1), H - 58, bottomW(2), 54, "BACK", A_GOTO, PAGE_SETTINGS);
    return;
  }
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

static int wifiRows() { return portrait() ? ROWS - 1 : ROWS; }   // (portrait has two rows of buttons)

static void pageWifi() {
  header("Choose your Wi-Fi network (2.4 GHz)", String(s_nets.size()) + " found");
  if (s_nets.empty()) centerText("No networks found - tap RESCAN", W / 2, 140, &fonts::Font2, C_DIM);
  int rows = wifiRows();
  if (s_netTop > max(0, (int)s_nets.size() - rows)) s_netTop = max(0, (int)s_nets.size() - rows);
  for (int r = 0; r < rows; r++) {
    int i = s_netTop + r;
    if (i >= (int)s_nets.size()) break;
    int y = ROW_Y + r * (ROW_H + 2);
    add(2, y, W - 4, ROW_H, "", A_WIFI_PICK, i, s_nets[i].ssid == s_eSsid);
    drawButton(s_btns.back(), false);
    fitText(s_nets[i].ssid, 12, y + 14, W - 120, &fonts::Font2, C_TEXT);
    if (!s_nets[i].open) drawLock(W - 70, y + 14);
    drawWifiBars(W - 44, y + 12, s_nets[i].rssi);
  }
  if (portrait()) {                                // two rows of buttons
    add(bottomX(3, 0), H - 116, bottomW(3), 54, "UP", A_LIST_UP);
    add(bottomX(3, 1), H - 116, bottomW(3), 54, "DOWN", A_LIST_DN);
    add(bottomX(3, 2), H - 116, bottomW(3), 54, "OTHER...", A_WIFI_OTHER);
    add(bottomX(2, 0), H - 58, bottomW(2), 54, "RESCAN", A_WIFI_RESCAN);
    add(bottomX(2, 1), H - 58, bottomW(2), 54, "BACK", A_GOTO, PAGE_SETUP);
    return;
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
  s_canvas.fillRoundRect(4, 20, W - 8, 32, 6, C_PANEL);
  String shown = secret ? String() : s_kbText;
  if (secret) for (size_t i = 0; i < s_kbText.length(); i++) shown += '*';
  s_canvas.setFont(&fonts::Font4);
  while (shown.length() && s_canvas.textWidth(shown.c_str()) > W - 40) shown.remove(0, 1);
  s_canvas.setTextColor(C_TEXT);
  s_canvas.drawString(shown.c_str(), 12, 24);
  int cx = 12 + s_canvas.textWidth(shown.c_str()) + 2;
  s_canvas.fillRect(cx, 24, 2, 24, C_HOLD);

  // portrait: narrower but taller keys
  const bool tall = portrait();
  const int KH2 = tall ? 64 : 46, R0 = tall ? 70 : 58, RS = tall ? 72 : 52;
  if (s_kbMode == KB_IP) {
    static const char* ip[] = {"123", "456", "789", ".0"};
    const int pitch = (W - 24) / 3 + 1, kw = pitch - 4;
    for (int r = 0; r < 4; r++)
      for (int c = 0; ip[r][c]; c++) add(12 + c * pitch, R0 + r * RS, kw, KH2, String(ip[r][c]), A_KB_CHAR, ip[r][c]);
    add(12 + 2 * pitch, R0 + 3 * RS, kw, KH2, "DEL", A_KB_BKSP);
  } else {
    const int pitch = tall ? 31 : 48, kw = pitch - 2;
    for (int r = 0; r < 3; r++) {
      const char* row = kbRows(s_kbMode, r);
      int n = strlen(row);
      int x0 = (r == 2) ? (tall ? (W - 7 * pitch) / 2 : 74) : (W - n * pitch) / 2;
      for (int c = 0; c < n; c++) add(x0 + c * pitch, R0 + r * RS, kw, KH2, String(row[c]), A_KB_CHAR, row[c]);
    }
    bool letters = s_kbMode == KB_LOWER || s_kbMode == KB_UPPER;
    int sw = tall ? (W - 7 * pitch) / 2 - 6 : 68;   // SHIFT / DEL either side of the last row
    add(2, R0 + 2 * RS, sw, KH2, letters ? "SHIFT" : (s_kbMode == KB_NUM ? "#+=" : "123"), A_KB_SHIFT, 0, s_kbMode == KB_UPPER);
    add(W - 2 - sw, R0 + 2 * RS, sw, KH2, "DEL", A_KB_BKSP);
    bool secretField = s_kbField == 1 || s_kbField == 3;
    int mw = tall ? 60 : 96, showW = tall ? 70 : 128;
    int spaceW = W - 2 - (mw + 6) - (secretField ? showW + 4 : 0) - 2;
    if (!tall && !secretField) spaceW = 244;          // (landscape layout as before)
    add(2, R0 + 3 * RS, mw, KH2, letters ? "123" : "ABC", A_KB_MODE);
    add(mw + 6, R0 + 3 * RS, spaceW, KH2, "space", A_KB_CHAR, ' ');
    if (secretField) add(W - 2 - showW, R0 + 3 * RS, showW, KH2, s_kbShow ? "HIDE" : "SHOW", A_KB_SHOW);
  }
  add(bottomX(2, 0), H - 54, bottomW(2), 50, "CANCEL", A_KB_CANCEL);
  add(bottomX(2, 1), H - 54, bottomW(2), 50, "OK", A_KB_OK, 0, false, C_GREENKEY);
}

// ---------- weather alert overlay ----------
static void drawAlert() {
  bool on = (millis() / 500) % 2;
  uint16_t bg = on ? C_BAD : C_DARKRED;
  int oy = (H - 320) / 2;                        // centred on a tall screen
  s_canvas.fillRoundRect(10, 50 + oy, W - 20, 170, 12, bg);
  s_canvas.drawRoundRect(10, 50 + oy, W - 20, 170, 12, C_TEXT);
  centerText("WEATHER ALERT", W / 2, 90 + oy, &fonts::FreeSansBold18pt7b, C_TEXT);
  const auto& st = Sds200::status();
  String f = st.freq; f.trim();
  String l = (st.channel.length() ? st.channel : String("NOAA Weather")) + (f.length() ? "   " + f : String(""));
  centerText(l, W / 2, 140 + oy, &fonts::FreeSans12pt7b, C_TEXT);
  centerText("Tap anywhere to dismiss", W / 2, 190 + oy, &fonts::Font2, C_TEXT);
}

// ---------- redraw ----------
static void drawPage() {
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
    int tw = min(360, W - 20), ty = 150 + (H - 320) / 2;
    s_canvas.fillRoundRect((W - tw) / 2, ty, tw, 40, 8, C_BTN_DOWN);
    centerText(s_toast, W / 2, ty + 20, &fonts::Font4, C_TEXT);
  }
}

// ---- region updates (big screens) ----
// The main page is split into regions. Each has a fingerprint of everything it shows;
// only regions whose fingerprint changed are cleared, redrawn (clipped) and sent to
// the screen. Anything else (other pages, pop-ups) redraws the whole screen.
struct Region { int x, y, w, h; };
static Region kMainRegions[3];   // top bar, information (incl. SCAN / WX row), bottom row: set by setLayout()
static String s_regionSig[3];
static bool s_regionsValid = false;

static String sigStatus() {
  const auto& st = Sds200::status();
  char clk[16] = "";
  clockText(clk, sizeof(clk));
  int wbars = WiFi.status() != WL_CONNECTED ? 0 : WiFi.RSSI() > -60 ? 3 : WiFi.RSSI() > -72 ? 2 : 1;
  return String(wbars) + "|" + st.online + clk + st.model + RtspAudio::connected() + AudioOut::playing() + Calls::replaying() +
         AudioOut::muted() + RtspAudio::status() + Calls::recordingNow() + AudioOut::volume() + "|" +
         Battery::percent() + "|" + st.sig;
}
static String sigInfo() {
  const auto& st = Sds200::status();
  String w = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("-");
  return String(st.online) + "|" + st.system + "|" + st.mode + "|" + st.department + "|" + st.channel + "|" + (st.sig > 0) +
         st.sysHold + st.deptHold + st.chanHold + "|" + st.tgid + "|" + st.freq + "|" + st.mod + "|" + st.service + "|" +
         st.site + "|" + st.rssi + "|" + st.info1 + "|" + st.info2 + "|" + st.popup + "|" + w + "|" + Settings::scannerIp() +
         RtspAudio::lockedOut() + rebootArmed() + Sds200::inWeather() + "|" + s_pressed + "|" + Settings::keyColor() +
         (portrait() ? "|" + String(AudioOut::volume()) + AudioOut::muted() : String(""));   // portrait shows the level here
}
static String sigButtons() { return String(AudioOut::muted()) + "|" + s_pressed + "|" + Settings::keyColor(); }

static bool overlayShowing() { return alertShowing() || (s_toast.length() && millis() < s_toastUntil); }

static bool redrawRegions() {
  static bool overlayWas = false;
  bool overlay = overlayShowing();
  bool can = s_canvas.scaled() && Board::bands() == 0 && s_page == PAGE_MAIN && !overlay && !overlayWas;
  overlayWas = overlay;
  if (!can) { s_regionsValid = false; return false; }
  String sig[3] = {sigStatus(), sigInfo(), sigButtons()};
  if (!s_regionsValid) {                          // first time on this page: full redraw, then regions
    for (int r = 0; r < 3; r++) s_regionSig[r] = sig[r];
    s_regionsValid = true;
    return false;
  }
  for (int r = 0; r < 3; r++) {
    if (sig[r] == s_regionSig[r]) continue;
    s_regionSig[r] = sig[r];
    const Region& g = kMainRegions[r];
    int x0 = s_canvas.X(g.x), y0 = s_canvas.Y(g.y), x1 = s_canvas.X(g.x + g.w), y1 = s_canvas.Y(g.y + g.h);
    s_canvas.c->setClipRect(x0, y0, x1 - x0, y1 - y0);
    s_canvas.clipping = true;
    s_canvas.cx0 = x0; s_canvas.cy0 = y0; s_canvas.cx1 = x1; s_canvas.cy1 = y1;
    drawPage();
    s_canvas.clipping = false;
    s_canvas.c->clearClipRect();
    Board::presentRect(x0, y0, x1 - x0, y1 - y0);
  }
  return true;
}

static void redraw() {
  s_fitCache.clear();
  if (redrawRegions()) return;
  int nb = Board::bands();
  if (nb == 0) {                                  // whole canvas at once
    drawPage();
    Board::present();
    return;
  }
  for (int k = 0; k < nb; k++) {                  // band by band (Tab5)
    int ox;
    s_canvas.c = &Board::band(k, ox);
    s_canvas.ox = ox;
    drawPage();
    Board::presentBand(k);
  }
  Board::bandsDone();
  s_canvas.c = &Board::canvas();
  s_canvas.ox = 0;
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
      if (!AudioOut::muted()) AudioOut::beep(880, 120);   // hear the new level right away
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
      else s_netTop = max(0, s_netTop - wifiRows());
      break;
    case A_LIST_DN:
      if (s_page == PAGE_RECENT) s_recentTop += ROWS;
      else s_netTop += wifiRows();
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
// Logical size, scale and the size-dependent layout for the canvas's current shape.
static void setLayout() {
  LGFX_Sprite& c = Board::canvas();
  s_canvas.c = &c;
  bool tall = c.height() > c.width();
  W = tall ? 320 : 480;
  H = tall ? 480 : 320;
  s_canvas.kx = c.width() / (float)W;
  s_canvas.ky = c.height() / (float)H;
  MBY = H - MBH - 2;
  MBW = (W - 4 - 3 * MBG) / 4;
  WX_TOP = MBY - MBH - MBG;
  WX_LEFT = MCOL(2);
  ROWS = (H - 56 - ROW_Y) / (ROW_H + 2);
  kMainRegions[0] = {0, 0, W, 24};
  kMainRegions[1] = {0, 24, W, MBY - 2 - 24};
  kMainRegions[2] = {0, MBY - 2, W, H - (MBY - 2)};
  for (auto& k : s_keyCache) delete k.img;        // cached keys were drawn at the old scale
  s_keyCache.clear();
  s_regionsValid = false;
}

bool begin() {
  if (!Board::beginDisplay()) return false;
  setLayout();
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
  s_canvas.setFont(&fonts::Font0);                // version in the bottom-right corner
  s_canvas.setTextDatum(textdatum_t::bottom_right);
  s_canvas.setTextColor(C_DIM);
  s_canvas.drawString("v" UC_VERSION, W - 4, H - 3);
  if (!status || !*status) { s_canvas.setTextDatum(textdatum_t::top_left); return; }
  s_canvas.setFont(&fonts::Font2);
  s_canvas.setTextDatum(textdatum_t::bottom_center);
  s_canvas.setTextColor(C_DIM);
  s_canvas.drawString(status, W / 2, H - 6);
  s_canvas.setTextDatum(textdatum_t::top_left);
}

void showLogo(const char* status) {
#if defined(BOARD_TAB5)
  {
    LGFX_Sprite& h = halfSprite();
    if (!halfJpeg(kLogo5Start, kLogo5End - kLogo5Start)) h.fillScreen(C_BG);
    halfStatus(status);
    Board::presentHalf(h);
    s_regionsValid = false;
    return;
  }
#endif
  s_canvas.fillScreen(C_BG);
  s_canvas.drawJpg(kLogoStart, kLogoEnd - kLogoStart, 0, 0);
  drawBootStatus(status);
  Board::present();
}

// boot_anim.bin: "UCA1", u16 frames, u16 ms/frame, then {u32 offset, u32 length} per frame, then JPEGs.
void bootAnimFrame(const char* status) {
  static uint16_t frame = 0;
  static uint32_t nextAt = 0;
#if defined(BOARD_TAB5)
  const uint8_t* a = kAnim5Start;
#else
  const uint8_t* a = kAnimStart;
#endif
  if (memcmp(a, "UCA1", 4) != 0) { showLogo(status); delay(100); return; }
  uint16_t count = a[4] | (a[5] << 8);
  uint16_t ms = a[6] | (a[7] << 8);
  const uint8_t* e = a + 8 + 8 * frame;
  uint32_t off = e[0] | (e[1] << 8) | (e[2] << 16) | ((uint32_t)e[3] << 24);
  uint32_t len = e[4] | (e[5] << 8) | (e[6] << 16) | ((uint32_t)e[7] << 24);
#if defined(BOARD_TAB5)
  LGFX_Sprite& h = halfSprite();
  if (!halfJpeg(a + off, len)) halfJpeg(kLogo5Start, kLogo5End - kLogo5Start);
  halfStatus(status);
  int32_t wait = (int32_t)(nextAt - millis());
  if (wait > 0 && wait <= ms) delay(wait);
  Board::presentHalf(h);
  s_regionsValid = false;
#else
  if (!s_canvas.drawJpg(a + off, len, 0, 0)) s_canvas.drawJpg(kLogoStart, kLogoEnd - kLogoStart, 0, 0);
  drawBootStatus(status);
  int32_t wait = (int32_t)(nextAt - millis());
  if (wait > 0 && wait <= ms) delay(wait);
  Board::present();
#endif
  // steady frame rate: the next frame is due one interval after this one was due
  uint32_t now = millis();
  nextAt = ((int32_t)(now - nextAt) > (int32_t)ms) ? now + ms : nextAt + ms;
  frame = (frame + 1) % count;
}

void showMessage(const char* line1, const char* line2) {
  s_regionsValid = false;                         // whole screen replaced
  s_canvas.fillScreen(C_BG);
  s_canvas.setTextDatum(textdatum_t::middle_center);
  s_canvas.setFont(&fonts::FreeSansBold18pt7b);
  s_canvas.setTextColor(C_SYS);
  int oy = (H - 320) / 2;
  s_canvas.drawString("Uniden Connect", W / 2, 110 + oy);
  s_canvas.setFont(&fonts::FreeSans12pt7b);
  s_canvas.setTextColor(C_TEXT);
  if (line1) s_canvas.drawString(line1, W / 2, 170 + oy);
  s_canvas.setFont(&fonts::Font2);
  s_canvas.setTextColor(C_DIM);
  if (line2) s_canvas.drawString(line2, W / 2, 205 + oy);
  s_canvas.setTextDatum(textdatum_t::top_left);
  Board::present();
}

// Serial debug helper: "S" sends a screenshot, "P<n>" shows page n, "B" beeps, "L" shows the boot
// logo, "O<n>" fixes the Tab5's picture at n quarter turns (0 landscape, 1 / 3 portrait; "O9" = automatic). Used for checking layouts from a computer.
static void serialDebug() {
  if (!Serial.available()) return;
  int c = Serial.read();
  if (c == 'S') {
    LGFX_Sprite& raw = Board::canvas();
    const uint8_t* p = (const uint8_t*)Board::frameData();
    size_t left = raw.bufferLength();
    if (s_canvas.scaled()) {                     // "SHT3", width, height, little-endian flag (u16 LE each), pixels
      uint16_t hdr[3] = {(uint16_t)Board::frameWidth(), (uint16_t)Board::frameHeight(), (uint16_t)Board::frameLittleEndian()};
      left = (size_t)hdr[0] * hdr[1] * 2;
      Serial.write((const uint8_t*)"SHT3", 4);
      Serial.write((const uint8_t*)hdr, 6);
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
  } else if (c == 'B') {                          // play the volume beep
    AudioOut::beep(880, 120);
  } else if (c == 'L') {                          // show the boot logo (checking colours)
    showLogo("logo test");
  } else if (c == 'O') {
    uint32_t t = millis();
    while (!Serial.available() && millis() - t < 200) delay(1);
    Board::setOrientation(Serial.read() - '0');
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

  if (Board::orientationChanged()) {               // the whole picture turned: redraw everything,
    s_pressed = -1;                                // not just the regions whose content changed
    setLayout();                                   // (and portrait / landscape may have swapped)
    s_dirty = true;
  }

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
  if ((s_dirty && now - s_lastDraw > UI_MIN_FRAME_MS) || now - s_lastDraw > 1000) {
    s_dirty = false;
    s_lastDraw = now;
    redraw();
  }
}

}  // namespace Ui
