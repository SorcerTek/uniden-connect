// Freenove FNK0104N: ST77922 QSPI panel (320x480 portrait, used rotated) + touch.
#if !defined(BOARD_TAB5)
#include "board.h"
#include "config.h"
#include "display_st77922.h"
#include "touch_st77922.h"
#include <esp_ota_ops.h>
#include <esp_partition.h>

namespace Board {

static LGFX_Sprite s_canvas;
// Mapping from canvas (landscape) to physical (portrait) pixels, measured at start-up:
// phys = origin + lx * dx + ly * dy
static int s_ox, s_oy, s_dxx, s_dxy, s_dyx, s_dyy;

static void measureRotation() {
  // Draw marker pixels at known canvas points and find where they land in the
  // portrait buffer. This gives the exact touch mapping for any rotation.
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
}

const char* name() { return "FNK0104N"; }

bool beginDisplay() {
  if (!St77922::begin()) return false;
  s_canvas.setPsram(true);
  s_canvas.setColorDepth(16);
  if (!s_canvas.createSprite(LCD_NATIVE_W, LCD_NATIVE_H)) return false;
  s_canvas.setRotation(SCREEN_ROTATION);
  measureRotation();
  return true;
}

LGFX_Sprite& canvas() { return s_canvas; }
void present() { St77922::pushFrame((const uint16_t*)s_canvas.getBuffer()); }
void presentRect(int, int, int, int) { present(); }
void presentHalf(LGFX_Sprite&) {}
void setBrightness(int pct) { St77922::setBrightness(pct); }

bool touch(int& x, int& y) {
  int16_t px, py;
  if (!Touch::read(px, py)) return false;
  int rx = px - s_ox, ry = py - s_oy;
  int det = s_dxx * s_dyy - s_dyx * s_dxy;
  if (det == 0) return false;
  x = (rx * s_dyy - ry * s_dyx) / det;
  y = (ry * s_dxx - rx * s_dxy) / det;
#if TOUCH_DEBUG
  Serial.printf("touch raw=(%d,%d) screen=(%d,%d)\n", px, py, x, y);
#endif
  return x >= 0 && x < s_canvas.width() && y >= 0 && y < s_canvas.height();
}

int frameWidth() { return LCD_NATIVE_W; }
int frameHeight() { return LCD_NATIVE_H; }
bool frameLittleEndian() { return false; }
const void* frameData() { return s_canvas.getBuffer(); }
int bands() { return 0; }
LGFX_Sprite& band(int, int& xOffset) { xOffset = 0; return s_canvas; }
void presentBand(int) {}
void bandsDone() {}

// No motion sensor: the orientation is chosen in SETTINGS (Landscape / Portrait / either
// flipped). The canvas is turned and the touch mapping measured again.
static int s_orient = 0;
static bool s_rotated = false;
int orientation() { return s_orient; }
bool autoRotates() { return false; }
void setOrientation(int k) {
  if (k < 0 || k > 3) k = 0;
  if (k == s_orient) return;
  s_orient = k;
  s_canvas.setRotation((((SCREEN_ROTATION & 3) + k) & 3) | (SCREEN_ROTATION & 4));
  measureRotation();
  s_rotated = true;
}
bool orientationChanged() { bool r = s_rotated; s_rotated = false; return r; }
bool hasBootButton() { return true; }
// bmorcelli's Launcher installs itself as the factory app and runs other firmware from an
// app slot. Running from a slot while a factory app exists = installed under the Launcher.
static const esp_partition_t* factoryPartition() {
  return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
}
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
#endif
