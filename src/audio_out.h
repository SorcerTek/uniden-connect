#pragma once
#include <Arduino.h>

// Speaker output: ES8311 codec + I2S + onboard amplifier, fed from a
// jitter buffer. Any thread can push decoded samples with push(); a
// background task resamples them to AUDIO_OUT_RATE and plays them.
namespace AudioOut {

bool begin();                                   // Wire must already be started
void setStreamRate(uint32_t hz);                // sample rate of incoming audio
void push(const int16_t* samples, size_t n);    // add mono samples
void flush();                                   // drop buffered audio
void setVolume(int percent);                    // 0..100 speaker volume
int  volume();
void setMuted(bool m);
bool muted();
bool playing();                                 // true while audio is flowing
uint32_t queued();                              // samples waiting in the buffer
uint32_t streamRate();
void beep(int hz = 880, int ms = 250);          // short tone (only when nothing else is playing)
void chime(bool up);                            // soft "ding" for the volume keys (higher for up)

// The chime: a bell-like note (fundamental plus two soft overtones) that fades in over
// 6 ms and dies away gently, instead of a beep's hard start and stop. Writes up to max
// samples at the given rate and peak level, returns how many.
inline size_t makeChime(int16_t* buf, size_t max, uint32_t rate, float hz, float peak) {
  size_t n = min<size_t>(max, rate * 450 / 1000);
  for (size_t i = 0; i < n; i++) {
    float t = (float)i / rate;
    float env = (t < 0.006f ? sinf(t / 0.006f * PI / 2) : expf(-(t - 0.006f) / 0.085f));
    if (i > n - rate / 100) env *= (float)(n - i) / (rate / 100);   // last 10 ms: down to zero
    float w = 2.0f * PI * hz * t;
    float v = sinf(w) + 0.30f * sinf(2 * w) * expf(-t / 0.05f) + 0.08f * sinf(3 * w) * expf(-t / 0.03f);
    buf[i] = (int16_t)(peak * env * v / 1.38f);
  }
  return n;
}

}  // namespace AudioOut
