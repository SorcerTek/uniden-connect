// ST77922 integrated touch, based on Freenove's ST77922_Touch driver and the
// register notes in github.com/emailman/Freenove_FNK0104N_MicroPython.
#include "touch_st77922.h"
#include "config.h"
#include <Wire.h>

namespace Touch {

static const uint16_t REG_STATUS = 0x0001;
static const uint16_t REG_MAX_TOUCHES = 0x0009;
static const uint16_t REG_TOUCH_INFO = 0x0010;
static const uint16_t REG_POINT0 = 0x0014;

static uint8_t s_maxPoints = 1;
static bool s_ok = false;

static bool readReg(uint16_t reg, uint8_t* buf, size_t len) {
  Wire.beginTransmission(TOUCH_I2C_ADDR);
  Wire.write((uint8_t)(reg >> 8));
  Wire.write((uint8_t)reg);
  if (Wire.endTransmission(false) != 0) return false;
  size_t got = Wire.requestFrom((uint16_t)TOUCH_I2C_ADDR, (size_t)len, true);
  if (got != len) return false;
  for (size_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

static void hwReset() {
  digitalWrite(PIN_TP_RST, LOW);
  delay(100);
  digitalWrite(PIN_TP_RST, HIGH);
  delay(100);
}

bool begin() {
  pinMode(PIN_TP_RST, OUTPUT);
  pinMode(PIN_TP_INT, INPUT);
  hwReset();

  uint8_t v = 0xFF;
  for (int i = 0; i < 200; i++) {
    if (readReg(REG_STATUS, &v, 1) && (v & 0x0F) == 0) break;
    delay(5);
  }
  if (readReg(REG_MAX_TOUCHES, &v, 1)) {
    s_maxPoints = (v >= 1 && v <= 10) ? v : 1;
    s_ok = true;
  }
  return s_ok;
}

bool read(int16_t& x, int16_t& y) {
  if (!s_ok) return false;
  uint8_t info;
  if (!readReg(REG_TOUCH_INFO, &info, 1)) return false;
  if (info & 0x80) {          // controller asks for a reset
    hwReset();
    return false;
  }
  if (!(info & 0x08)) return false;   // no coordinates

  // Must read the whole point block or the controller keeps reporting a touch.
  uint8_t buf[7 * 10];
  if (!readReg(REG_POINT0, buf, 7 * s_maxPoints)) return false;
  if (!(buf[0] & 0x80)) return false;

  int16_t rx = ((buf[0] & 0x3F) << 8) | buf[1];
  int16_t ry = ((buf[2] & 0x3F) << 8) | buf[3];
#if TOUCH_SWAP_XY
  { int16_t t = rx; rx = ry * LCD_NATIVE_W / LCD_NATIVE_H; ry = t * LCD_NATIVE_H / LCD_NATIVE_W; }
#endif
#if TOUCH_INVERT_X
  rx = LCD_NATIVE_W - 1 - rx;
#endif
#if TOUCH_INVERT_Y
  ry = LCD_NATIVE_H - 1 - ry;
#endif
  x = rx;
  y = ry;
  return true;
}

}  // namespace Touch
