// Small RTSP/RTP client for the SDS200 audio stream.
//
//   OPTIONS -> DESCRIBE (read SDP) -> SETUP (UDP client_port) -> PLAY
//   then keep-alive OPTIONS before the session timeout, and receive RTP.
//
// Supports G.711 u-law (PCMU), A-law (PCMA) and 16-bit linear (L16) payloads.
// The SDP the scanner sends is printed on the Serial Monitor for troubleshooting.
#include "rtsp_audio.h"
#include "audio_out.h"
#include "calls.h"
#include "config.h"
#include "settings.h"
#include <WiFi.h>
#include <WiFiUdp.h>

namespace RtspAudio {

enum Codec { C_NONE, C_PCMU, C_PCMA, C_L16 };

static volatile bool s_connected = false;
static char s_status[40] = "starting";
static char s_codecName[24] = "-";

static void setStatus(const char* s) {
  strncpy(s_status, s, sizeof(s_status) - 1);
  s_status[sizeof(s_status) - 1] = 0;
}

static volatile bool s_reconnect = false;
static volatile uint32_t s_busySince = 0;
static volatile bool s_shutdown = false;
static volatile bool s_stopped = false;

bool connected() { return s_connected; }
void reconnect() { s_reconnect = true; s_busySince = 0; }
bool lockedOut() { return s_busySince && millis() - s_busySince > 30000; }
void shutdown(uint32_t waitMs) {
  s_shutdown = true;
  s_reconnect = true;
  uint32_t start = millis();
  while (!s_stopped && millis() - start < waitMs) delay(10);
}

// Wait up to ms, returning early if a reconnect was requested.
static void waitOrReconnect(uint32_t ms) {
  uint32_t start = millis();
  while (!s_reconnect && millis() - start < ms) delay(20);
}
const char* status() { return s_status; }
const char* codecName() { return s_codecName; }

// ---------- G.711 decoders ----------
static int16_t ulaw2lin(uint8_t u) {
  u = ~u;
  int t = ((u & 0x0F) << 3) + 0x84;
  t <<= (u & 0x70) >> 4;
  return (u & 0x80) ? (0x84 - t) : (t - 0x84);
}
static int16_t alaw2lin(uint8_t a) {
  a ^= 0x55;
  int t = (a & 0x0F) << 4;
  int seg = (a & 0x70) >> 4;
  switch (seg) {
    case 0: t += 8; break;
    case 1: t += 0x108; break;
    default: t += 0x108; t <<= seg - 1;
  }
  return (a & 0x80) ? t : -t;
}

// ---------- RTSP helpers ----------
struct Session {
  WiFiClient tcp;
  WiFiUDP rtp;
  String base;        // rtsp://ip:port/path
  String control;     // URL for SETUP
  String aggregate;   // URL for PLAY / OPTIONS / TEARDOWN (Content-Base or base)
  bool setupDone = false;
  String session;     // Session id
  uint32_t timeoutS = 60;
  int cseq = 1;
  Codec codec = C_NONE;
  uint8_t payloadType = 0;
  uint32_t rate = 8000;
};

// Send a request and read the response. Returns status code (or -1).
static int request(Session& s, const String& method, const String& url, const String& extra, String* headers, String* body) {
  String req = method + " " + url + " RTSP/1.0\r\n";
  req += "CSeq: " + String(s.cseq++) + "\r\n";
  req += "User-Agent: UnidenConnect/1.0\r\n";
  if (s.session.length()) req += "Session: " + s.session + "\r\n";
  req += extra;
  req += "\r\n";
  s.tcp.print(req);

  uint32_t start = millis();
  String hdr;
  // read header block
  while (millis() - start < 4000) {
    if (!s.tcp.connected() && !s.tcp.available()) return -1;
    if (!s.tcp.available()) { delay(5); continue; }
    String line = s.tcp.readStringUntil('\n');
    if (line == "\r" || line.length() == 0) {
      if (hdr.length()) break;
      continue;
    }
    hdr += line + "\n";
  }
  if (!hdr.startsWith("RTSP/1.0 ")) return -1;
  int code = hdr.substring(9, 12).toInt();

  int clen = 0;
  int i = hdr.indexOf("Content-Length:");
  if (i < 0) i = hdr.indexOf("Content-length:");
  if (i >= 0) clen = hdr.substring(i + 15, hdr.indexOf('\n', i)).toInt();
  String b;
  while ((int)b.length() < clen && millis() - start < 5000) {
    if (s.tcp.available()) b += (char)s.tcp.read();
    else delay(2);
  }
  if (headers) *headers = hdr;
  if (body) *body = b;
  return code;
}

static String headerValue(const String& hdrs, const char* name) {
  String lower = hdrs;
  lower.toLowerCase();
  String n = String(name);
  n.toLowerCase();
  int i = lower.indexOf(n + ":");
  if (i < 0) return "";
  int e = hdrs.indexOf('\n', i);
  String v = hdrs.substring(i + n.length() + 1, e);
  v.trim();
  return v;
}

static bool parseSdp(Session& s, const String& sdp, const String& contentBase) {
  s.codec = C_NONE;
  String audioControl;
  bool inAudio = false;
  int pos = 0;
  while (pos < (int)sdp.length()) {
    int e = sdp.indexOf('\n', pos);
    if (e < 0) e = sdp.length();
    String line = sdp.substring(pos, e);
    line.trim();
    pos = e + 1;

    if (line.startsWith("m=")) {
      inAudio = line.startsWith("m=audio");
      if (inAudio) {
        // m=audio 0 RTP/AVP 0
        int sp = line.lastIndexOf(' ');
        s.payloadType = line.substring(sp + 1).toInt();
        if (s.payloadType == 0) { s.codec = C_PCMU; s.rate = 8000; }
        if (s.payloadType == 8) { s.codec = C_PCMA; s.rate = 8000; }
        if (s.payloadType == 11) { s.codec = C_L16; s.rate = 44100; }
      }
    } else if (inAudio && line.startsWith("a=rtpmap:")) {
      // a=rtpmap:<pt> <name>/<rate>[/ch]
      int sp = line.indexOf(' ');
      String enc = line.substring(sp + 1);
      String name = enc.substring(0, enc.indexOf('/'));
      String rest = enc.substring(enc.indexOf('/') + 1);
      uint32_t rate = rest.toInt();
      name.toUpperCase();
      if (name == "PCMU") s.codec = C_PCMU;
      else if (name == "PCMA") s.codec = C_PCMA;
      else if (name == "L16") s.codec = C_L16;
      if (rate) s.rate = rate;
    } else if (inAudio && line.startsWith("a=control:")) {
      audioControl = line.substring(10);
    }
  }

  String base = s.aggregate;
  if (audioControl.length() == 0 || audioControl == "*") s.control = base;
  else if (audioControl.startsWith("rtsp://")) s.control = audioControl;
  else s.control = base + (base.endsWith("/") ? "" : "/") + audioControl;

  const char* n = s.codec == C_PCMU ? "PCMU" : s.codec == C_PCMA ? "PCMA" : s.codec == C_L16 ? "L16" : "?";
  snprintf(s_codecName, sizeof(s_codecName), "%s/%u", n, (unsigned)s.rate);
  return s.codec != C_NONE;
}

static bool startSession(Session& s) {
  s.cseq = 1;
  s.session = "";
  s.setupDone = false;
  // Leave the port out of the URL when it is the default (the form ProScan / VLC use).
  String ip = Settings::scannerIp();
  if (SCANNER_RTSP_PORT == 554) s.base = String("rtsp://") + ip + SCANNER_RTSP_PATH;
  else s.base = String("rtsp://") + ip + ":" + SCANNER_RTSP_PORT + SCANNER_RTSP_PATH;
  s.aggregate = s.base;

  setStatus("connecting");
  if (!s.tcp.connect(ip.c_str(), SCANNER_RTSP_PORT, 3000)) {
    // "Connection reset" = the scanner is still holding an old listener (e.g. the board
    // was reset or unplugged mid-stream). It only lets go when the scanner restarts.
    if (!s_busySince) s_busySince = millis();
    setStatus(errno == ECONNRESET ? "scanner busy" : "no RTSP server");
    return false;
  }
  s_busySince = 0;
  s.tcp.setTimeout(3);

  String hdr, body;
  int code = request(s, "OPTIONS", s.base, "", &hdr, nullptr);
  if (code != 200) { setStatus("OPTIONS failed"); log_w("OPTIONS -> %d", code); return false; }

  code = request(s, "DESCRIBE", s.base, "Accept: application/sdp\r\n", &hdr, &body);
  if (code != 200) { setStatus("DESCRIBE failed"); log_w("DESCRIBE -> %d\n%s", code, hdr.c_str()); return false; }
  Serial.println("---- SDS200 SDP ----");
  Serial.println(body);
  Serial.println("--------------------");
  String cb = headerValue(hdr, "Content-Base");
  cb.trim();
  if (cb.length()) s.aggregate = cb;
  if (!parseSdp(s, body, cb)) { setStatus("unknown codec"); return false; }
  Serial.printf("RTSP aggregate URL: %s\nRTSP track URL: %s\n", s.aggregate.c_str(), s.control.c_str());

  s.rtp.stop();
  if (!s.rtp.begin(RTP_LOCAL_PORT)) { setStatus("UDP bind failed"); return false; }

  String transport = String("Transport: RTP/AVP;unicast;client_port=") + RTP_LOCAL_PORT + "-" + (RTP_LOCAL_PORT + 1) + "\r\n";
  code = request(s, "SETUP", s.control, transport, &hdr, nullptr);
  if (code != 200) { setStatus("SETUP failed"); log_w("SETUP -> %d\n%s", code, hdr.c_str()); return false; }
  Serial.printf("---- SETUP response ----\n%s------------------------\n", hdr.c_str());
  String sess = headerValue(hdr, "Session");
  int semi = sess.indexOf(';');
  if (semi >= 0) {
    int t = sess.indexOf("timeout=");
    if (t >= 0) s.timeoutS = max(10, (int)sess.substring(t + 8).toInt());
    sess = sess.substring(0, semi);
  }
  sess.trim();
  s.session = sess;
  s.setupDone = true;

  // PLAY: try a few URL / header forms, stop at the first 200.
  struct { char tag; String url; bool range; } tries[] = {
    {'a', s.aggregate, false},
    {'b', s.aggregate, true},
    {'c', s.aggregate + "/", false},
    {'d', s.control, false},
  };
  bool played = false;
  for (auto& t : tries) {
    if (t.tag == 'c' && s.aggregate.endsWith("/")) continue;
    code = request(s, "PLAY", t.url, t.range ? "Range: npt=0.000-\r\n" : "", &hdr, nullptr);
    Serial.printf("RTSP PLAY (%c) %s range=%s -> %d\n", t.tag, t.url.c_str(), t.range ? "yes" : "no", code);
    if (code == 200) { Serial.printf("PLAY OK via (%c)\n", t.tag); played = true; break; }
    Serial.printf("%s\n", hdr.c_str());
    if (code < 0) break;   // connection gone
  }
  if (!played) { setStatus("PLAY failed"); return false; }

  AudioOut::setStreamRate(s.rate);
  AudioOut::flush();
  setStatus("streaming");
  return true;
}

static void stopSession(Session& s) {
  if (s.setupDone && s.tcp.connected()) request(s, "TEARDOWN", s.aggregate, "", nullptr, nullptr);
  s.setupDone = false;
  s.tcp.stop();
  s.rtp.stop();
  s.session = "";
  s_connected = false;
}

static void rtspTask(void*) {
  Session s;
  static uint8_t pkt[1600];
  static int16_t pcm[1600];

  uint32_t backoffS = 3;
  for (;;) {
    if (s_shutdown) { s_stopped = true; vTaskDelete(nullptr); }
    if (WiFi.status() != WL_CONNECTED) { setStatus("waiting for Wi-Fi"); delay(1000); continue; }
    s_reconnect = false;
    if (!startSession(s)) {
      char why[40];
      strncpy(why, s_status, sizeof(why) - 1);
      why[sizeof(why) - 1] = 0;
      stopSession(s);
      // Back off: 3 s, doubling up to 60 s. RECONNECT AUDIO retries immediately.
      uint32_t until = millis() + backoffS * 1000;
      while (!s_reconnect && (int32_t)(until - millis()) > 0) {
        char buf[40];
        snprintf(buf, sizeof(buf), "%s, retry in %us", why, (unsigned)((until - millis() + 999) / 1000));
        setStatus(buf);
        delay(200);
      }
      if (s_reconnect) backoffS = 3;
      else backoffS = min<uint32_t>(backoffS * 2, 60);
      continue;
    }
    backoffS = 3;
    s_connected = true;

    uint32_t lastRtp = millis();
    uint32_t lastKeepAlive = millis();
    while (true) {
      int len = s.rtp.parsePacket();
      if (len > 0) {
        len = s.rtp.read(pkt, sizeof(pkt));
        if (len > 12 && (pkt[0] >> 6) == 2) {
          int off = 12 + (pkt[0] & 0x0F) * 4;            // CSRC list
          if (pkt[0] & 0x10 && len > off + 4) {           // header extension
            off += 4 + ((pkt[off + 2] << 8) | pkt[off + 3]) * 4;
          }
          int plen = len - off;
          if (pkt[0] & 0x20 && plen > 0) plen -= pkt[len - 1];   // padding
          if (plen > 0) {
            const uint8_t* p = pkt + off;
            int n = 0;
            if (s.codec == C_PCMU) for (int i = 0; i < plen; i++) pcm[n++] = ulaw2lin(p[i]);
            else if (s.codec == C_PCMA) for (int i = 0; i < plen; i++) pcm[n++] = alaw2lin(p[i]);
            else if (s.codec == C_L16) for (int i = 0; i + 1 < plen; i += 2) pcm[n++] = (int16_t)((p[i] << 8) | p[i + 1]);
            Calls::feed(pcm, n, s.rate);                 // recorder / weather-alert detector
            if (!Calls::replaying()) AudioOut::push(pcm, n);
          }
          lastRtp = millis();
        }
      } else {
        delay(2);
      }

      // Drain anything the server sends on the control connection.
      while (s.tcp.available()) s.tcp.read();

      if (millis() - lastKeepAlive > s.timeoutS * 1000 / 2) {
        lastKeepAlive = millis();
        if (request(s, "OPTIONS", s.aggregate, "", nullptr, nullptr) < 0) { setStatus("RTSP lost"); break; }
      }
      // The scanner may send nothing while squelched; only treat a long gap with
      // a dead TCP connection as a failure.
      if (!s.tcp.connected()) { setStatus("RTSP closed"); break; }
      if (millis() - lastRtp > 120000) { setStatus("no audio, retrying"); break; }
      if (WiFi.status() != WL_CONNECTED) break;
      if (s_reconnect) { setStatus("reconnecting"); break; }
    }
    stopSession(s);
    delay(1000);
  }
}

void begin() {
  xTaskCreatePinnedToCore(rtspTask, "rtsp", 8192, nullptr, 10, nullptr, 0);
}

}  // namespace RtspAudio
