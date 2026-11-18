#pragma once
#include <Arduino.h>

// Call detection, call log, recording and weather-alert detection.
//
// The live audio (from RtspAudio) is copied into a buffer here and examined in
// 50 ms blocks by a low-priority task:
//   - a "call" starts when real audio is heard (level above REC_SILENCE_RMS while the
//     scanner is online) and ends after REC_HANG_MS of silence or a channel change;
//     calls shorter than REC_MIN_MS are dropped
//   - each call goes into the RECENT list, /logs/YYYY-MM-DD.csv on the SD card and,
//     when recording is on, /rec/YYYY-MM-DD/HHMMSS_<channel>.wav
//   - a 1050 Hz tone lasting WX_ALERT_TONE_MS raises the weather alert
namespace Calls {

struct Call {
  time_t start;          // 0 if the clock wasn't set yet
  uint32_t startMs;      // millis() at start
  uint32_t durMs;
  String system, dept, channel, tgid, freq;
  String wav;            // path on the SD card, "" if not recorded
};

void begin();                                   // mount the SD card, start the task
void feed(const int16_t* pcm, size_t n, uint32_t rate);   // live audio (any task)

// What the scanner is on right now (call from the main loop when GSI changes).
void setContext(bool online, const String& system, const String& dept, const String& channel,
                const String& tgid, const String& freq);

bool sdOk();
String sdInfo();                                // e.g. "SD 29.1 GB free" / "no SD card"
bool recordingNow();                            // a WAV file is being written
bool inCall();                                  // audio is coming through right now

int recentCount();
bool recent(int i, Call& out);                  // 0 = newest
uint32_t recentVersion();                       // changes when the list changes

bool alertActive();                             // weather alert tone heard (until ack)
void ackAlert();

bool play(int i);                               // replay recent call i through the speaker
void stopPlay();
bool replaying();

void closeAll();                                // finish files (before power off / restart / update)

}  // namespace Calls
