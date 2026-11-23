#if !defined(BOARD_TAB5)   // FNK0104N only (Tab5 version: board_tab5.cpp)
// ES8311 setup is a trimmed port of Espressif's esp-bsp es8311 driver
// (Apache-2.0), using its clock coefficient entries for 4.096 MHz MCLK.
#include "audio_out.h"
#include "config.h"
#include <Wire.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>
#include <atomic>

namespace AudioOut {

// ---------------- ES8311 ----------------
static bool codecWrite(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(CODEC_I2C_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static uint8_t codecRead(uint8_t reg) {
  Wire.beginTransmission(CODEC_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom((uint16_t)CODEC_I2C_ADDR, (size_t)1, true) != 1) return 0;
  return Wire.read();
}

struct Coeff { uint32_t mclk, rate; uint8_t pre_div, pre_multi, adc_div, dac_div, fs_mode, lrck_h, lrck_l, bclk_div, adc_osr, dac_osr; };
// From esp-bsp es8311.c coeff_div[]
static const Coeff kCoeffs[] = {
  {4096000, 16000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
  {2048000,  8000, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00, 0xff, 0x04, 0x10, 0x10},
};

static bool codecInit(uint32_t rate) {
  const Coeff* c = nullptr;
  for (auto& k : kCoeffs) if (k.rate == rate && k.mclk == rate * 256) c = &k;
  if (!c) return false;

  if (!codecWrite(0x00, 0x1F)) return false;          // reset
  delay(20);
  codecWrite(0x00, 0x00);
  codecWrite(0x00, 0x80);                              // power on, slave mode
  codecWrite(0x01, 0x3F);                              // all clocks on, MCLK from MCLK pin

  uint8_t r06 = codecRead(0x06) & ~(1 << 5);           // SCLK not inverted
  codecWrite(0x06, r06);

  uint8_t r02 = codecRead(0x02) & 0x07;
  r02 |= (c->pre_div - 1) << 5;
  r02 |= c->pre_multi << 3;
  codecWrite(0x02, r02);
  codecWrite(0x03, (c->fs_mode << 6) | c->adc_osr);
  codecWrite(0x04, c->dac_osr);
  codecWrite(0x05, ((c->adc_div - 1) << 4) | (c->dac_div - 1));
  r06 = codecRead(0x06) & 0xE0;
  r06 |= (c->bclk_div < 19) ? (c->bclk_div - 1) : c->bclk_div;
  codecWrite(0x06, r06);
  codecWrite(0x07, (codecRead(0x07) & 0xC0) | c->lrck_h);
  codecWrite(0x08, c->lrck_l);

  codecWrite(0x00, codecRead(0x00) & 0xBF);            // slave serial port
  codecWrite(0x09, 3 << 2);                            // SDP in: I2S, 16-bit
  codecWrite(0x0A, 3 << 2);                            // SDP out: I2S, 16-bit

  codecWrite(0x0D, 0x01);   // power up analog
  codecWrite(0x0E, 0x02);   // enable analog PGA / ADC modulator
  codecWrite(0x12, 0x00);   // power up DAC
  codecWrite(0x13, 0x10);   // enable output drive
  codecWrite(0x1C, 0x6A);   // ADC EQ bypass
  codecWrite(0x37, 0x08);   // DAC EQ bypass
  return true;
}

static void codecVolume(int pct) {
  pct = constrain(pct, 0, 100);
  codecWrite(0x32, pct == 0 ? 0 : (pct * 256 / 100) - 1);
}

static void codecMute(bool m) {
  uint8_t r = codecRead(0x31);
  if (m) r |= (1 << 6) | (1 << 5); else r &= ~((1 << 6) | (1 << 5));
  codecWrite(0x31, r);
}

// ---------------- jitter buffer ----------------
static const size_t RING = 32768;            // samples (~4 s at 8 kHz), power of two
static int16_t* s_ring = nullptr;
static std::atomic<uint32_t> s_head{0};      // write index (producer)
static std::atomic<uint32_t> s_tail{0};      // read index (consumer)
static std::atomic<uint32_t> s_streamRate{8000};
static std::atomic<bool> s_flush{false};
static std::atomic<uint32_t> s_lastAudioMs{0};
static int s_volume = SPEAKER_VOLUME_DEFAULT;
static bool s_muted = false;

void push(const int16_t* samples, size_t n) {
  uint32_t h = s_head.load(std::memory_order_relaxed);
  uint32_t t = s_tail.load(std::memory_order_acquire);
  for (size_t i = 0; i < n; i++) {
    if (h - t >= RING - 1) break;          // full: drop the rest
    s_ring[h & (RING - 1)] = samples[i];
    h++;
  }
  s_head.store(h, std::memory_order_release);
  s_lastAudioMs.store(millis());
}

void flush() { s_flush = true; }
void setStreamRate(uint32_t hz) { if (hz >= 4000 && hz <= 48000) s_streamRate = hz; }

static int16_t* s_chime = nullptr;               // chime samples (see chime())
static std::atomic<uint32_t> s_chimeLen{0}, s_chimePos{0};
static std::atomic<uint32_t> s_toneLeft{0};      // tone samples still to play (see beep())
static std::atomic<uint32_t> s_toneStep{0};      // 16.16 phase step per output sample

static void audioTask(void*) {
  const int FRAMES = 256;                    // output frames per write
  static int16_t out[FRAMES * 2];            // stereo
  bool primed = false;
  uint32_t phase = 0;                        // 16.16 fixed-point position between samples
  int16_t prev = 0, cur = 0;

  for (;;) {
    if (s_flush.exchange(false)) {
      s_tail.store(s_head.load());
      primed = false;
    }
    uint32_t rate = s_streamRate.load();
    uint32_t avail = s_head.load(std::memory_order_acquire) - s_tail.load();
    uint32_t prebuf = rate * AUDIO_PREBUFFER_MS / 1000;

    if (!primed && avail >= prebuf) primed = true;
    // If we fall far behind (network burst), skip ahead to keep latency low.
    if (primed && avail > prebuf * 4) s_tail.fetch_add(avail - prebuf);

    uint32_t step = (uint32_t)(((uint64_t)rate << 16) / AUDIO_OUT_RATE);
    for (int i = 0; i < FRAMES; i++) {
      int16_t s = 0;
      if (primed) {
        while (phase >= 0x10000) {
          uint32_t t = s_tail.load();
          if (s_head.load(std::memory_order_acquire) == t) { primed = false; break; }   // underrun
          prev = cur;
          cur = s_ring[t & (RING - 1)];
          s_tail.store(t + 1, std::memory_order_release);
          phase -= 0x10000;
        }
        int32_t frac = phase & 0xFFFF;
        s = (int16_t)(prev + (((int32_t)(cur - prev) * frac) >> 16));
        phase += step;
      }
      if (s_toneLeft.load()) {                 // volume / alert tone
        static uint32_t tph = 0;
        tph += s_toneStep.load();
        int32_t v = s + (int32_t)(9000 * sinf(2.0f * PI * (tph & 0xFFFF) / 65536.0f));
        s = (int16_t)constrain(v, -32768, 32767);
        s_toneLeft--;
      }
      uint32_t cp = s_chimePos.load();
      if (cp < s_chimeLen.load()) {            // volume-key chime
        s = (int16_t)constrain((int32_t)s + s_chime[cp], -32768, 32767);
        s_chimePos = cp + 1;
      }
      out[i * 2] = s;
      out[i * 2 + 1] = s;
    }
    size_t written;
    i2s_write(I2S_NUM_0, out, sizeof(out), &written, portMAX_DELAY);
  }
}

bool begin() {
  s_ring = (int16_t*)heap_caps_malloc(RING * sizeof(int16_t), MALLOC_CAP_SPIRAM);
  if (!s_ring) s_ring = (int16_t*)malloc(RING * sizeof(int16_t));
  if (!s_ring) return false;

  pinMode(PIN_AMP_EN, OUTPUT);
  digitalWrite(PIN_AMP_EN, HIGH);            // amp off while we set up

  i2s_config_t cfg = {};
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
  cfg.sample_rate = AUDIO_OUT_RATE;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.intr_alloc_flags = 0;
  cfg.dma_buf_count = 8;
  cfg.dma_buf_len = 256;
  cfg.use_apll = false;
  cfg.tx_desc_auto_clear = true;
  cfg.fixed_mclk = AUDIO_OUT_RATE * 256;
  cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  if (i2s_driver_install(I2S_NUM_0, &cfg, 0, nullptr) != ESP_OK) return false;

  i2s_pin_config_t pins = {};
  pins.mck_io_num = PIN_I2S_MCLK;
  pins.bck_io_num = PIN_I2S_BCLK;
  pins.ws_io_num = PIN_I2S_WS;
  pins.data_out_num = PIN_I2S_DOUT;
  pins.data_in_num = I2S_PIN_NO_CHANGE;
  if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) return false;
  i2s_zero_dma_buffer(I2S_NUM_0);
  delay(10);   // let MCLK run before talking to the codec

  if (!codecInit(AUDIO_OUT_RATE)) {
    log_e("ES8311 codec not responding at 0x%02X", CODEC_I2C_ADDR);
    return false;
  }
  codecVolume(s_volume);
  digitalWrite(PIN_AMP_EN, LOW);             // amp on (active LOW)

  xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, 20, nullptr, 0);
  return true;
}

void setVolume(int pct) { s_volume = constrain(pct, 0, 100); codecVolume(s_volume); }
int volume() { return s_volume; }
void setMuted(bool m) {
  s_muted = m;
  codecMute(m);
  digitalWrite(PIN_AMP_EN, m ? HIGH : LOW);
}
bool muted() { return s_muted; }
bool playing() { return millis() - s_lastAudioMs.load() < 1500; }
uint32_t queued() { return s_head.load() - s_tail.load(); }
uint32_t streamRate() { return s_streamRate.load(); }

// Short tone mixed straight into the speaker output (does not wait for the jitter buffer).
void chime(bool up) {
  static const size_t MAX = AUDIO_OUT_RATE / 2;
  if (!s_chime) s_chime = (int16_t*)malloc(MAX * sizeof(int16_t));
  if (!s_chime) return;
  s_chimeLen = 0;                              // stop the one playing while the samples change
  s_chimePos = 0;
  s_chimeLen = makeChime(s_chime, MAX, AUDIO_OUT_RATE, up ? 698.5f : 523.3f, 6500);   // F5 up, C5 down
}

void beep(int hz, int ms) {
  s_toneStep = (uint32_t)(((uint64_t)hz << 16) / AUDIO_OUT_RATE);
  s_toneLeft = (uint32_t)AUDIO_OUT_RATE * ms / 1000;
}

}  // namespace AudioOut
#endif  // !BOARD_TAB5
