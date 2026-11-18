#pragma once
// ===============================================================
//  Uniden Connect - settings you might want to change
// ===============================================================

// ---- Your scanner ----------------------------------------------
#define SCANNER_IP          "192.168.1.2"
#define SCANNER_CTRL_PORT   50536            // SDS200 "virtual serial" UDP port
#define SCANNER_RTSP_PORT   554
#define SCANNER_RTSP_PATH   "/au:scanner.au" // SDS200 audio stream path
#define SCANNER_REBOOT_MSM_S 5               // "Reboot scanner" sends MSM,<this> (seconds in mass-storage mode)

// How often to ask the scanner for its status (milliseconds)
#define STATUS_POLL_MS      400

// ---- Audio -----------------------------------------------------
#define RTP_LOCAL_PORT      5004             // UDP port the board receives audio on
#define AUDIO_OUT_RATE      16000            // speaker sample rate (stream is resampled to this)
#define AUDIO_PREBUFFER_MS  250              // buffering before playback starts (raise if choppy)
#define SPEAKER_VOLUME_DEFAULT 70            // 0-100, board speaker volume (separate from scanner VOL)

// ---- Screen ----------------------------------------------------
// 1 or 3. Flip between them if the picture is upside down.
#define SCREEN_ROTATION     1
#define LCD_SPI_HZ          40000000         // 40 MHz is the safe value for this panel
// Set to 1 if colours look wrong (red and blue swapped / garbled).
#define LCD_SWAP_BYTES      0

// Touch fine-tuning (only if taps land in the wrong place).
// Turn on TOUCH_DEBUG to print raw touch coordinates on the Serial Monitor.
#define TOUCH_DEBUG         0
#define TOUCH_INVERT_X      0
#define TOUCH_INVERT_Y      0
#define TOUCH_SWAP_XY       0

// ===============================================================
//  Board wiring - Freenove FNK0104N. You should not need to change these.
// ===============================================================
// ST77922 LCD, 4-lane QSPI, 320x480 native portrait
#define PIN_LCD_CS    10
#define PIN_LCD_SCK   12
#define PIN_LCD_D0    11
#define PIN_LCD_D1    13
#define PIN_LCD_D2    14
#define PIN_LCD_D3    9
#define PIN_LCD_BL    41   // backlight, active HIGH
#define LCD_NATIVE_W  320
#define LCD_NATIVE_H  480

// Shared I2C bus: touch controller (0x55) and ES8311 audio codec (0x18)
#define PIN_I2C_SDA   38
#define PIN_I2C_SCL   39
#define PIN_TP_RST    48
#define PIN_TP_INT    47
#define TOUCH_I2C_ADDR 0x55
#define CODEC_I2C_ADDR 0x18

// I2S to ES8311 codec
#define PIN_I2S_MCLK  17
#define PIN_I2S_BCLK  18
#define PIN_I2S_WS    21
#define PIN_I2S_DOUT  15
#define PIN_AMP_EN    1    // speaker amplifier enable, active LOW

// BOOT button: hold it this long to open the power menu (POWER OFF / RESTART)
#define PIN_BOOT_BTN  0
#define POWER_HOLD_MS 1500

// Battery sense (ADC) through a resistor divider
#define PIN_BATTERY_ADC  8
#define BATTERY_DIVIDER  2.0f  // adjust if the battery % looks wrong
#define BATTERY_DEBUG    0     // 1 = print battery mV / % on the Serial Monitor each second
