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

}  // namespace AudioOut
