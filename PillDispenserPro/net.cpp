// Wi-Fi (station + setup hotspot with captive portal), mDNS, NTP time zone
#include "app.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

static DNSServer dns;
static bool ap = false, mdnsStarted = false, wasConnected = false, everConnected = false;
static uint32_t apStopAt = 0, lastRetry = 0;
static String apS, apP;
static uint8_t netIdx = 0;
static uint32_t scanHoldUntil = 0;  // no reconnect attempts while a scan runs (the ESP32 cannot scan while connecting)
// reconnect logic: quick retries that slow down gradually, and a full radio restart if it stays offline
static uint8_t retryCount = 0;
static uint32_t offlineSince = 0, lastRadioReset = 0;
static volatile uint8_t lastReason = 0;  // last disconnect reason from the Wi-Fi driver
static volatile uint32_t dropAt = 0;

String apSsid() { return apS; }
String apPass() { return apP; }
bool apActive() { return ap; }
bool netConnected() { return WiFi.status() == WL_CONNECTED; }

String localIp() {
  if (netConnected()) return WiFi.localIP().toString();
  if (ap) return WiFi.softAPIP().toString();
  return "0.0.0.0";
}

void applyTimezone() {
  configTzTime(cfg.tz.c_str(), "pool.ntp.org", "time.google.com", "time.nist.gov");
}

void startAP() {
  if (NET_OFF || ap) return;
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char s[24], p[12];
  snprintf(s, sizeof(s), "PillDispenser-%02X%02X", mac[4], mac[5]);
  snprintf(p, sizeof(p), "pd%02x%02x%02x", mac[3], mac[4], mac[5]);
  apS = s;
  apP = p;
  WiFi.mode(cfg.ssid.length() ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(apS.c_str(), apP.c_str());
  dns.start(53, "*", WiFi.softAPIP());
  ap = true;
  apStopAt = 0;
  Serial.printf("Setup hotspot %s / %s\n", s, p);
}

static void stopAP() {
  if (!ap) return;
  dns.stop();
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  ap = false;
}

void connectWifi(const String& ssid, const String& pass) {
  if (NET_OFF || !ssid.length()) return;
  WiFi.mode(ap ? WIFI_AP_STA : WIFI_STA);
  WiFi.setSleep(false);         // no modem power-save: phone hotspots drop sleepy clients
  WiFi.disconnect(false, false);  // clear a half-finished attempt before starting a new one
  WiFi.begin(ssid.c_str(), pass.c_str());
  lastRetry = millis();
}

static const char* reasonText(uint8_t r) {
  switch (r) {
    case 15: case 202: case 204: return "wrong password?";
    case 2: return "authentication expired (usual while the hotspot starts)";
    case 201: return "network not found (hotspot off or 5 GHz?)";
    case 200: case 8: return "signal lost";
    case 203: return "association failed";
    case 3: case 4: case 6: case 7: return "the router/hotspot dropped us";
  }
  return "";
}

static bool haveNetwork() { return cfg.ssid.length() || cfg.ssid2.length(); }

// alternate between the main and the backup network until one connects
static void connectNext(bool advance) {
  if (advance && cfg.ssid.length() && cfg.ssid2.length()) netIdx ^= 1;
  if (netIdx == 1 && !cfg.ssid2.length()) netIdx = 0;
  if (netIdx == 0 && !cfg.ssid.length()) netIdx = 1;
  if (netIdx) connectWifi(cfg.ssid2, cfg.pass2);
  else connectWifi(cfg.ssid, cfg.pass);
}

void netInit() {
  if (NET_OFF) {  // no network: clock starts at the firmware build time
    setenv("TZ", cfg.tz.c_str(), 1);
    tzset();
    static const char* mon = "JanFebMarAprMayJunJulAugSepOctNovDec";
    struct tm t = {};
    char m[4] = {__DATE__[0], __DATE__[1], __DATE__[2], 0};
    t.tm_mon = (strstr(mon, m) - mon) / 3;
    t.tm_mday = atoi(__DATE__ + 4);
    t.tm_year = atoi(__DATE__ + 7) - 1900;
    t.tm_hour = atoi(__TIME__);
    t.tm_min = atoi(__TIME__ + 3);
    t.tm_sec = atoi(__TIME__ + 6) + 40;  // ~flash + boot time
    t.tm_isdst = -1;
    struct timeval tv{mktime(&t), 0};
    settimeofday(&tv, nullptr);
    return;
  }
  WiFi.persistent(false);
  WiFi.setHostname(cfg.hostname.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    lastReason = info.wifi_sta_disconnected.reason;
    dropAt = millis();
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  applyTimezone();
  if (haveNetwork()) connectNext(false);
  else startAP();
}

void netTick() {
  if (NET_OFF) return;
  if (ap) dns.processNextRequest();
  bool c = netConnected();
  if (c && !wasConnected) {
    everConnected = true;
    if (!mdnsStarted && MDNS.begin(cfg.hostname.c_str())) {
      MDNS.addService("http", "tcp", 80);
      mdnsStarted = true;
    }
    if (ap) apStopAt = millis() + 120000;  // keep hotspot briefly so the phone sees the success page
    uiToast(String(LV_SYMBOL_WIFI " Connected  ") + WiFi.localIP().toString());
    Serial.printf("Wi-Fi connected to %s, %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
  }
  if (!c && wasConnected) {
    Serial.printf("Wi-Fi lost (reason %u %s)%s", lastReason, reasonText(lastReason), "\n");
    offlineSince = millis();
    retryCount = 0;
    lastRetry = millis() - 60000;  // try again right away
  }
  if (c) { retryCount = 0; offlineSince = 0; }
  else if (!offlineSince) offlineSince = millis();
  wasConnected = c;
  if (!c && haveNetwork() && (int32_t)(millis() - scanHoldUntil) > 0) {
    // 3 s, 6 s, 9 s ... up to 30 s between attempts (45 s while the setup hotspot is on)
    uint32_t wait = min<uint32_t>(3000UL * (retryCount + 1), ap ? 45000UL : 30000UL);
    if (millis() - lastRetry > wait) {
      // offline for 5 min: restart the Wi-Fi radio completely (the ESP32 stack can get stuck)
      if (millis() - offlineSince > 300000UL && millis() - lastRadioReset > 300000UL) {
        lastRadioReset = millis();
        Serial.println("Wi-Fi: offline 5 min, restarting the radio");
        WiFi.mode(WIFI_OFF);
        delay(300);
        WiFi.mode(ap ? WIFI_AP_STA : WIFI_STA);
        if (ap) WiFi.softAP(apS.c_str(), apP.c_str());
      }
      if (retryCount < 255) retryCount++;
      connectNext(retryCount % 2 == 0);  // with a backup network: alternate between the two
    }
  }
  if (ap && apStopAt && c && millis() > apStopAt) stopAP();
  // saved network never reachable since boot -> offer the setup hotspot (after 3 min: it slows reconnecting)
  if (!everConnected && !ap && millis() > 180000) startAP();
}

void scanStart() {
  if (NET_OFF) return;
  if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) return;
  scanHoldUntil = millis() + 15000;
  if (!netConnected()) { WiFi.disconnect(); delay(100); }
  if (WiFi.scanNetworks(true) == WIFI_SCAN_FAILED) { delay(300); WiFi.scanNetworks(true); }
}

int scanResults(String& json) {
  if (NET_OFF) { json = "[]"; return 0; }
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return -1;
  if (n < 0) { scanStart(); return -1; }
  json = "[";
  int count = 0;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    bool dup = false;
    for (int j = 0; j < i; j++) if (WiFi.SSID(j) == s) { dup = true; break; }
    if (dup) continue;
    s.replace("\\", "\\\\");
    s.replace("\"", "\\\"");
    json += String(count ? "," : "") + "{\"ssid\":\"" + s + "\",\"rssi\":" + WiFi.RSSI(i) +
            ",\"open\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
    count++;
  }
  json += "]";
  WiFi.scanDelete();
  return count;
}

// USB diagnostics ('D' on the serial port): saved networks, connection state and what the dispenser can see
void netDiag() {
  static const char* const ST[] = {"idle", "no SSID found", "scan done", "CONNECTED", "connect failed (wrong password?)", "connection lost", "disconnected"};
  int st = WiFi.status();
  Serial.printf("\nDIAG last drop: reason %u %s, %lu s ago, retries %u\n", lastReason, reasonText(lastReason),
                dropAt ? (unsigned long)((millis() - dropAt) / 1000) : 0UL, retryCount);
  Serial.printf("DIAG wifi main='%s' (pass %u chars) backup='%s' status=%d %s ip=%s rssi=%d ap=%d %s\n",
                cfg.ssid.c_str(), cfg.pass.length(), cfg.ssid2.c_str(), st, st >= 0 && st <= 6 ? ST[st] : "?",
                localIp().c_str(), netConnected() ? WiFi.RSSI() : 0, ap, apS.c_str());
  Serial.printf("DIAG clock %s, telegram token %s (%u chars), patient ids '%s', family ids '%s', AI key %s\n",
                timeValid() ? "OK" : "NOT SET", cfg.tgToken.length() ? "saved" : "EMPTY", cfg.tgToken.length(),
                cfg.tgPatient.c_str(), cfg.tgCare.c_str(), cfg.aiKey.length() ? "saved" : "empty");
  if (NET_OFF) { Serial.println("DIAG network is OFF (safe mode or NO_WIFI)"); return; }
  if (netConnected()) {  // is there real internet behind the Wi-Fi, and does Telegram accept the token?
    IPAddress ip;
    bool dns = WiFi.hostByName("api.telegram.org", ip);
    WiFiClient c;
    bool web = c.connect("connectivitycheck.gstatic.com", 80, 4000);
    c.stop();
    Serial.printf("DIAG internet: DNS %s (%s), web %s\n", dns ? "OK" : "FAILED", ip.toString().c_str(), web ? "OK" : "FAILED");
    if (cfg.tgToken.length() && timeValid()) {
      WiFiClientSecure t;
      t.setInsecure();
      HTTPClient h;
      h.setTimeout(8000);
      if (h.begin(t, "https://api.telegram.org/bot" + cfg.tgToken + "/getMe")) {
        int code = h.GET();
        String r = code > 0 ? h.getString() : String("");
        h.end();
        Serial.printf("DIAG telegram getMe: HTTP %d %s\n", code, r.substring(0, 160).c_str());
      }
    }
  }
  if (!netConnected()) { WiFi.disconnect(); delay(200); }
  int n = WiFi.scanNetworks(false, true);
  if (n < 0) { delay(500); n = WiFi.scanNetworks(false, true); }
  Serial.printf("DIAG scan: %d networks\n", n);
  for (int i = 0; i < n; i++)
    Serial.printf("DIAG  %-32s ch%-2d %4d dBm %s\n", WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i),
                  WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "");
  WiFi.scanDelete();
  Serial.println("DIAG end");
}
