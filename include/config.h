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

// ---- Clock -----------------------------------------------------
// POSIX time zone (US Central with daylight saving). Eastern: "EST5EDT,M3.2.0,M11.1.0"
#define TIMEZONE            "CST6CDT,M3.2.0,M11.1.0"
#define NTP_SERVER_1        "pool.ntp.org"
#define NTP_SERVER_2        "time.nist.gov"

// ---- Recording / call log (microSD) ----------------------------
// A call is recorded only while real audio comes through: the audio level must be
// above REC_SILENCE_RMS (16-bit sample RMS over 50 ms). Raise it if hiss gets
// recorded, lower it if quiet calls are missed.
#define REC_SILENCE_RMS     300
#define REC_HANG_MS         2000             // a call ends after this much silence
#define REC_MIN_MS          1000             // shorter calls are thrown away
#define RECENT_MAX          20               // calls kept in the RECENT list

// ---- Weather alert ----------------------------------------------
// NOAA Weather Radio warning tone (1050 Hz) heard for at least this long = alert.
#define WX_ALERT_TONE_MS    1000

// ---- Wireless updates -------------------------------------------
#define OTA_HOSTNAME        "uniden-connect"   // upload to uniden-connect.local

// ---- Screen ----------------------------------------------------
#define DIM_AFTER_S_DEFAULT 60               // dim after this many idle seconds (changeable in SETTINGS)
#define DIM_LEVEL_DEFAULT   10               // dimmed brightness % (0 = off)
// Boot animation plays while connecting: at least this long (one pass of the
// animation), and no longer than the timeout even if the scanner isn't answering.
#define BOOT_ANIM_MIN_MS      3000
#define BOOT_ANIM_TIMEOUT_MS  25000

// Shortest time between screen redraws. The Tab5's big screen shares slow PSRAM with
// the display controller, so it redraws less often.
#if defined(BOARD_TAB5)
#define UI_MIN_FRAME_MS     40
#else
#define UI_MIN_FRAME_MS     40
#endif

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

// ---- M5Stack Tab5 (only used by the tab5 build) ------------------
#define CHIME_MAX_VOLUME    80               // volume-key chime: never louder than at this SPK level (%)
#define TAB5_ROTATION       3                // 1 or 3: landscape; flip if upside down
#define TAB5_AUTO_ROTATE    1                // 1 = turn the picture with the Tab5 (IMU)
#define TAB5_PORTRAIT       1                // 1 = switch to the portrait layout when held upright
#define TAB5_PORTRAIT_SIDE  0                // 0 / 1: swap if portrait comes out upside down
#define TAB5_FB_CHUNK       4096             // screen writes: bytes per piece ...
#define TAB5_FB_GAP_US      30               // ... and pause after each (avoids display underruns)
#define TAB5_SPK_GAIN       12               // speaker / headphone digital gain (M5Unified default 4)
#define TAB5_BANDED         0                // 1 = draw in bands in internal RAM, 0 = full canvas in PSRAM
#define TAB5_DMA_BURST      16               // DMA burst size for screen copies (8-128)
#define TAB5_SD_SCK         43               // microSD in SPI mode
#define TAB5_SD_MISO        39
#define TAB5_SD_MOSI        44
#define TAB5_SD_CS          42
// ESP32-C6 Wi-Fi chip on SDIO (ESP-Hosted): CLK, CMD, D0-D3, RESET
#define TAB5_WIFI_SDIO_PINS 12, 13, 11, 10, 9, 8, 15

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

// microSD card (SDMMC, 4-bit)
#define PIN_SD_CLK    5
#define PIN_SD_CMD    4
#define PIN_SD_D0     6
#define PIN_SD_D1     7
#define PIN_SD_D2     2
#define PIN_SD_D3     3

// BOOT button: hold it this long to open the power menu (POWER OFF / RESTART)
#define PIN_BOOT_BTN  0
#define POWER_HOLD_MS 1500

// Battery sense (ADC) through a resistor divider
#define PIN_BATTERY_ADC  8
#define BATTERY_DIVIDER  2.0f  // adjust if the battery % looks wrong
#define BATTERY_DEBUG    0     // 1 = print battery mV / % on the Serial Monitor each second
