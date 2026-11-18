// SDS200 network control.
// Protocol per Uniden "SDS200 Virtual Serial on Network Specification" and
// "SDS Series Remote Command Specification":
//  - UDP port 50536, no header, one command per packet, terminated by '\r'
//  - XML replies (GSI) may span several packets; each starts "GSI,<XML>," and
//    the last one has <Footer ... EOT="1"/> (or ends with </ScannerInfo>).
#include "sds200.h"
#include "config.h"
#include <WiFi.h>
#include <WiFiUdp.h>

namespace Sds200 {

static WiFiUDP s_udp;
static IPAddress s_ip;
static Status s_st;
static String s_xml;
static uint32_t s_lastPoll = 0, s_lastReply = 0, s_xmlStart = 0;
static bool s_started = false;

const Status& status() { return s_st; }

void sendRaw(const char* cmd) {
  if (!s_started) return;
  s_udp.beginPacket(s_ip, SCANNER_CTRL_PORT);
  s_udp.write((const uint8_t*)cmd, strlen(cmd));
  s_udp.write((uint8_t)'\r');
  s_udp.endPacket();
}

void key(char code, char mode) {
  char buf[16];
  snprintf(buf, sizeof(buf), "KEY,%c,%c", code, mode);
  sendRaw(buf);
  s_lastPoll = 0;   // refresh status soon
}

void setVolume(int level) {
  char buf[16];
  snprintf(buf, sizeof(buf), "VOL,%d", constrain(level, 0, 29));
  sendRaw(buf);
  if (s_st.vol >= 0) { s_st.vol = constrain(level, 0, 29); s_st.version++; }
}

void setSquelch(int level) {
  char buf[16];
  snprintf(buf, sizeof(buf), "SQL,%d", constrain(level, 0, 19));
  sendRaw(buf);
  if (s_st.sql >= 0) { s_st.sql = constrain(level, 0, 19); s_st.version++; }
}

// Same trick ProScan uses: ask for mass-storage mode for a few seconds; when it
// times out the scanner restarts, which also restarts its RTSP audio server.
void rebootScanner() {
  char buf[16];
  snprintf(buf, sizeof(buf), "MSM,%d", SCANNER_REBOOT_MSM_S);
  sendRaw(buf);
  log_w("sent %s (reboot scanner)", buf);
}

// ---------------- tiny XML attribute helpers ----------------
static String unescape(String s) {
  s.replace("&amp;", "&");
  s.replace("&lt;", "<");
  s.replace("&gt;", ">");
  s.replace("&quot;", "\"");
  s.replace("&apos;", "'");
  s.replace("\\n", " ");
  s.trim();
  return s;
}

// Returns the text of element <tag ...> (start-tag only), or "" if absent.
static String element(const String& xml, const char* tag) {
  String open = String("<") + tag + " ";
  int i = xml.indexOf(open);
  if (i < 0) {
    open = String("<") + tag + ">";
    i = xml.indexOf(open);
    if (i < 0) return "";
  }
  int e = xml.indexOf('>', i);
  return e < 0 ? "" : xml.substring(i, e);
}

static String attr(const String& el, const char* name) {
  if (!el.length()) return "";
  String key = String(" ") + name + "=\"";
  int i = el.indexOf(key);
  if (i < 0) {
    key = String(" ") + name + " = \"";
    i = el.indexOf(key);
    if (i < 0) return "";
  }
  i += key.length();
  int e = el.indexOf('"', i);
  return e < 0 ? "" : unescape(el.substring(i, e));
}

template <typename T>
static void set(T& field, const T& v, bool& changed) {
  if (!(field == v)) { field = v; changed = true; }
}

static void parseGsi(const String& xml) {
  bool ch = false;
  String si = element(xml, "ScannerInfo");
  set(s_st.mode, attr(si, "Mode"), ch);
  set(s_st.vScreen, attr(si, "V_Screen"), ch);
  set(s_st.favorite, attr(element(xml, "MonitorList"), "Name"), ch);

  String sys = element(xml, "System");
  set(s_st.system, attr(sys, "Name"), ch);
  set(s_st.sysHold, attr(sys, "Hold") == "On", ch);

  String dept = element(xml, "Department");
  set(s_st.department, attr(dept, "Name"), ch);
  set(s_st.deptHold, attr(dept, "Hold") == "On", ch);

  set(s_st.site, attr(element(xml, "Site"), "Name"), ch);

  // The "current channel" element depends on what the scanner is doing.
  static const char* chanTags[] = {"ConvFrequency", "TGID", "SiteFrequency", "SrchFrequency",
                                   "CcHitsChannel", "ToneOutChannel", "WxChannel"};
  String cel;
  for (auto t : chanTags) { cel = element(xml, t); if (cel.length()) break; }
  set(s_st.channel, attr(cel, "Name"), ch);
  String f = attr(cel, "Freq");
  set(s_st.freq, f, ch);
  set(s_st.tgid, attr(cel, "TGID"), ch);
  set(s_st.mod, attr(cel, "Mod"), ch);
  set(s_st.service, attr(cel, "SvcType"), ch);
  set(s_st.chanHold, attr(cel, "Hold") == "On", ch);
  set(s_st.chanAvoid, attr(cel, "Avoid") != "" && attr(cel, "Avoid") != "Off", ch);

  String prop = element(xml, "Property");
  if (prop.length()) {
    String v = attr(prop, "VOL"); if (v.length()) set(s_st.vol, (int)v.toInt(), ch);
    v = attr(prop, "SQL");        if (v.length()) set(s_st.sql, (int)v.toInt(), ch);
    v = attr(prop, "Sig");        set(s_st.sig, (int)v.toInt(), ch);
    set(s_st.rssi, attr(prop, "Rssi"), ch);
    set(s_st.scannerMuted, attr(prop, "Mute") == "Mute", ch);
  }

  set(s_st.info1, attr(element(xml, "InfoArea1"), "Text"), ch);
  set(s_st.info2, attr(element(xml, "InfoArea2"), "Text"), ch);
  set(s_st.popup, attr(element(xml, "PopupScreen"), "Text"), ch);

  if (ch) s_st.version++;
}

static void handlePacket(const char* data, int len) {
  String pkt;
  pkt.reserve(len + 1);
  for (int i = 0; i < len; i++) pkt += data[i];
  s_lastReply = millis();
  if (!s_st.online) { s_st.online = true; s_st.version++; }

  if (pkt.startsWith("GSI,")) {
    // strip the "GSI,<XML>," prefix line of every fragment
    int nl = pkt.indexOf('\r');
    int nl2 = pkt.indexOf('\n');
    if (nl < 0 || (nl2 >= 0 && nl2 < nl)) nl = nl2;
    if (s_xml.length() == 0) s_xmlStart = millis();
    s_xml += (nl >= 0) ? pkt.substring(nl + 1) : pkt.substring(4);
    if (s_xml.indexOf("EOT=\"1\"") >= 0 || s_xml.indexOf("</ScannerInfo>") >= 0) {
      parseGsi(s_xml);
      s_xml = "";
    }
    return;
  }
  if (pkt.startsWith("MDL,")) {
    String m = pkt.substring(4);
    m.trim();
    if (m != s_st.model) { s_st.model = m; s_st.version++; }
    return;
  }
  // Other replies (KEY,OK / VOL,n / SQL,n ...) are just logged.
  pkt.trim();
  log_d("scanner: %s", pkt.c_str());
}

void begin() {
  s_ip.fromString(SCANNER_IP);
  s_udp.begin(0);   // any local port
  s_started = true;
  sendRaw("MDL");
}

void loop() {
  if (!s_started || WiFi.status() != WL_CONNECTED) return;
  static char buf[1500];

  int len;
  while ((len = s_udp.parsePacket()) > 0) {
    len = s_udp.read((uint8_t*)buf, sizeof(buf));
    if (len > 0) handlePacket(buf, len);
  }

  // Give up on a half-received XML reply.
  if (s_xml.length() && millis() - s_xmlStart > 1500) {
    parseGsi(s_xml);   // use what we have
    s_xml = "";
  }

  uint32_t now = millis();
  if (now - s_lastPoll >= STATUS_POLL_MS) {
    s_lastPoll = now;
    if (s_xml.length() == 0) sendRaw("GSI");
    if (s_st.model.length() == 0 && (now / 1000) % 5 == 0) sendRaw("MDL");
  }
  if (s_st.online && now - s_lastReply > 3000) {
    s_st.online = false;
    s_st.version++;
  }
}

}  // namespace Sds200
