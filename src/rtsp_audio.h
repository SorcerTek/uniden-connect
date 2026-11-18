#pragma once
#include <Arduino.h>

// Pulls the SDS200's RTSP audio stream (RTP over UDP) and feeds AudioOut.
namespace RtspAudio {

void begin();                 // starts a background task that keeps the stream connected
bool connected();             // RTSP session is up
const char* status();         // short human-readable state for the screen
const char* codecName();      // e.g. "PCMU/8000"
bool lockedOut();             // scanner has refused us for 30 s+ (old listener stuck; reboot scanner)
void reconnect();             // drop the session and start a fresh one now
void shutdown(uint32_t waitMs = 1500);  // TEARDOWN the session and stop for good (power off / restart)

}  // namespace RtspAudio
