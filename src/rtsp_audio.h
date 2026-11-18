#pragma once
#include <Arduino.h>

// Pulls the SDS200's RTSP audio stream (RTP over UDP) and feeds AudioOut.
namespace RtspAudio {

void begin();                 // starts a background task that keeps the stream connected
bool connected();             // RTSP session is up
const char* status();         // short human-readable state for the screen
const char* codecName();      // e.g. "PCMU/8000"

}  // namespace RtspAudio
