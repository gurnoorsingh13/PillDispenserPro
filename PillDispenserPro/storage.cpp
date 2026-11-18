// Config / state persistence (LittleFS on the "spiffs" partition) and dose history log
#include "app.h"
#include <LittleFS.h>
#include <Preferences.h>
#include <ArduinoJson.h>

static const char* CFG_FILE = "/config.json";
static const char* ST_FILE = "/state.json";
static const char* LOG_FILE = "/log.bin";
static const char* LOG_OLD = "/log1.bin";
static const size_t LOG_MAX = 40000;  // bytes per file (5000 records), two files kept
static const char* MASK = "********";

static bool fsOk = false;
static bool psDirty = false;
static uint32_t psDirtyAt = 0;

void storageInit() {
  fsOk = LittleFS.begin(true, "/littlefs", 6, "spiffs");
  Serial.printf("LittleFS %s\n", fsOk ? "ok" : "FAILED");
}

// ---------------- helpers ----------------
template <typename T>
static void getNum(JsonVariantConst v, T& dst, long lo, long hi) {
  if (v.isNull()) return;
  long x = v.as<long>();
  dst = (T)constrain(x, lo, hi);
}
static void getBool(JsonVariantConst v, bool& dst) {
  if (!v.isNull()) dst = v.as<bool>();
}
static void getStr(JsonVariantConst v, String& dst, size_t maxLen, bool secret = false) {
  if (v.isNull()) return;
  String s = v.as<String>();
  if (secret && s == MASK) return;  // unchanged secret
  s.trim();
  if (s.length() > maxLen) s = s.substring(0, maxLen);
  dst = s;
}

// ---------------- config <-> json ----------------
void configToJson(String& out, bool secrets) {
  JsonDocument d;
  auto sec = [&](const String& s) { return secrets ? s : (s.length() ? String(MASK) : String("")); };
  d["hostname"] = cfg.hostname;
  d["trays"] = cfg.trays;
  d["tz"] = cfg.tz;
  d["tzName"] = cfg.tzName;
  d["volume"] = cfg.volume;
  d["mute"] = cfg.mute;
  d["brightness"] = cfg.brightness;
  d["invert"] = cfg.invert;
  d["clockSaver"] = cfg.clockSaver;
  d["screenTimeout"] = cfg.screenTimeout;
  d["quiet"] = cfg.quiet;
  d["quietFrom"] = cfg.quietFrom;
  d["quietTo"] = cfg.quietTo;
  d["quietVolume"] = cfg.quietVolume;
  d["remindEvery"] = cfg.remindEvery;
  d["snoozeMin"] = cfg.snoozeMin;
  d["escalateAfter"] = cfg.escalateAfter;
  d["missAfter"] = cfg.missAfter;
  d["requireConfirm"] = cfg.requireConfirm;
  d["confirmTimeout"] = cfg.confirmTimeout;
  d["minGap"] = cfg.minGap;
  d["earlyWindow"] = cfg.earlyWindow;
  d["maxPerDay"] = cfg.maxPerDay;
  d["checkin"] = cfg.checkin;
  d["checkinH"] = cfg.checkinH;
  d["checkinM"] = cfg.checkinM;
  d["ntfyTopic"] = cfg.ntfyTopic;
  d["waPhone"] = cfg.waPhone;
  d["waKey"] = sec(cfg.waKey);
  d["callUser"] = cfg.callUser;
  d["hookUrl"] = cfg.hookUrl;
  d["aiKey"] = sec(cfg.aiKey);
  d["aiModel"] = cfg.aiModel;
  d["aiVision"] = cfg.aiVision;
  d["ssid2"] = cfg.ssid2;
  d["pass2"] = sec(cfg.pass2);
  d["capacity"] = cfg.capacity;
  d["lowStockDays"] = cfg.lowStockDays;
  d["pin"] = sec(cfg.pin);
  d["pinDispense"] = cfg.pinDispense;
  d["pinSettings"] = cfg.pinSettings;
  d["webPass"] = sec(cfg.webPass);
  d["tgToken"] = sec(cfg.tgToken);
  d["tgPatient"] = cfg.tgPatient;
  d["tgCare"] = cfg.tgCare;
  d["tgDue"] = cfg.tgDue;
  d["tgDaily"] = cfg.tgDaily;
  d["dailyH"] = cfg.dailyH;
  d["dailyM"] = cfg.dailyM;
  d["servoRest"] = cfg.servoRest;
  d["servoPush"] = cfg.servoPush;
  d["pushMs"] = cfg.pushMs;
  d["restMs"] = cfg.restMs;
  d["cyclesPerDose"] = cfg.cyclesPerDose;
  d["ssid"] = cfg.ssid;
  d["pass"] = sec(cfg.pass);
  JsonArray pa = d["people"].to<JsonArray>();
  for (int i = 0; i < MAX_PEOPLE; i++) pa.add(cfg.people[i]);
  JsonArray ta = d["tray"].to<JsonArray>();
  for (int t = 1; t <= MAX_TRAYS; t++) {
    const TrayCfg& tc = cfg.tray[t];
    JsonObject o = ta.add<JsonObject>();
    o["name"] = tc.name;
    o["person"] = tc.person;
    o["alerts"] = tc.alerts;
    o["voice"] = tc.voice;
    JsonArray sa = o["slots"].to<JsonArray>();
    for (int s = 0; s < MAX_SLOTS; s++) {
      JsonObject so = sa.add<JsonObject>();
      so["on"] = tc.slot[s].on;
      so["h"] = tc.slot[s].h;
      so["m"] = tc.slot[s].m;
      so["days"] = tc.slot[s].days;
      so["note"] = tc.slot[s].note;
    }
  }
  out = "";
  serializeJson(d, out);
}

bool configFromJson(const char* json, String& err) {
  JsonDocument d;
  DeserializationError e = deserializeJson(d, json);
  if (e) { err = e.c_str(); return false; }
  JsonVariantConst r = d.as<JsonVariantConst>();
  getStr(r["hostname"], cfg.hostname, 24);
  cfg.hostname.replace(" ", "-");
  if (!cfg.hostname.length()) cfg.hostname = "pilldisp";
  getNum(r["trays"], cfg.trays, 1, MAX_TRAYS);
  getStr(r["tz"], cfg.tz, 60);
  getStr(r["tzName"], cfg.tzName, 40);
  getNum(r["volume"], cfg.volume, 0, 30);
  getBool(r["mute"], cfg.mute);
  getNum(r["brightness"], cfg.brightness, 5, 100);
  getBool(r["invert"], cfg.invert);
  getBool(r["clockSaver"], cfg.clockSaver);
  getNum(r["screenTimeout"], cfg.screenTimeout, 15, 3600);
  getBool(r["quiet"], cfg.quiet);
  getNum(r["quietFrom"], cfg.quietFrom, 0, 23);
  getNum(r["quietTo"], cfg.quietTo, 0, 23);
  getNum(r["quietVolume"], cfg.quietVolume, 0, 30);
  getNum(r["remindEvery"], cfg.remindEvery, 1, 30);
  getNum(r["snoozeMin"], cfg.snoozeMin, 1, 60);
  getNum(r["escalateAfter"], cfg.escalateAfter, 0, 600);
  getNum(r["missAfter"], cfg.missAfter, 10, 720);
  getBool(r["requireConfirm"], cfg.requireConfirm);
  getNum(r["confirmTimeout"], cfg.confirmTimeout, 1, 240);
  getNum(r["minGap"], cfg.minGap, 0, 1440);
  getNum(r["earlyWindow"], cfg.earlyWindow, 0, 240);
  getNum(r["maxPerDay"], cfg.maxPerDay, 0, 8);
  getBool(r["checkin"], cfg.checkin);
  getNum(r["checkinH"], cfg.checkinH, 0, 23);
  getNum(r["checkinM"], cfg.checkinM, 0, 59);
  getStr(r["ntfyTopic"], cfg.ntfyTopic, 64);
  getStr(r["waPhone"], cfg.waPhone, 20);
  getStr(r["waKey"], cfg.waKey, 20, true);
  getStr(r["callUser"], cfg.callUser, 40);
  getStr(r["hookUrl"], cfg.hookUrl, 160);
  getStr(r["aiKey"], cfg.aiKey, 120, true);
  getStr(r["aiModel"], cfg.aiModel, 60);
  getStr(r["aiVision"], cfg.aiVision, 60);
  if (!cfg.aiModel.length()) cfg.aiModel = "openai/gpt-oss-120b";
  if (!cfg.aiVision.length()) cfg.aiVision = "qwen/qwen3.8-27b";
  getStr(r["ssid2"], cfg.ssid2, 32);
  getStr(r["pass2"], cfg.pass2, 64, true);
  if (cfg.callUser.length() && cfg.callUser[0] != '@') cfg.callUser = "@" + cfg.callUser;
  getNum(r["capacity"], cfg.capacity, 1, 60);
  getNum(r["lowStockDays"], cfg.lowStockDays, 0, 30);
  getStr(r["pin"], cfg.pin, 6, true);
  getBool(r["pinDispense"], cfg.pinDispense);
  getBool(r["pinSettings"], cfg.pinSettings);
  getStr(r["webPass"], cfg.webPass, 32, true);
  getStr(r["tgToken"], cfg.tgToken, 80, true);
  getStr(r["tgPatient"], cfg.tgPatient, 80);
  getStr(r["tgCare"], cfg.tgCare, 80);
  getBool(r["tgDue"], cfg.tgDue);
  getBool(r["tgDaily"], cfg.tgDaily);
  getNum(r["dailyH"], cfg.dailyH, 0, 23);
  getNum(r["dailyM"], cfg.dailyM, 0, 59);
  getNum(r["servoRest"], cfg.servoRest, 0, 180);
  getNum(r["servoPush"], cfg.servoPush, 0, 180);
  getNum(r["pushMs"], cfg.pushMs, 100, 2000);
  getNum(r["restMs"], cfg.restMs, 100, 2000);
  getNum(r["cyclesPerDose"], cfg.cyclesPerDose, 1, 6);
  getStr(r["ssid"], cfg.ssid, 32);
  getStr(r["pass"], cfg.pass, 64, true);
  for (char c : cfg.pin) if (c < '0' || c > '9') { cfg.pin = ""; break; }

  JsonArrayConst pa = r["people"];
  for (int i = 0; i < MAX_PEOPLE && i < (int)pa.size(); i++) getStr(pa[i], cfg.people[i], 18);
  JsonArrayConst ta = r["tray"];
  for (int t = 1; t <= MAX_TRAYS && t - 1 < (int)ta.size(); t++) {
    JsonObjectConst o = ta[t - 1];
    TrayCfg& tc = cfg.tray[t];
    getStr(o["name"], tc.name, 22);
    getNum(o["person"], tc.person, 0, MAX_PEOPLE - 1);
    getBool(o["alerts"], tc.alerts);
    getNum(o["voice"], tc.voice, 0, 99);
    JsonArrayConst sa = o["slots"];
    for (int s = 0; s < MAX_SLOTS && s < (int)sa.size(); s++) {
      getBool(sa[s]["on"], tc.slot[s].on);
      getNum(sa[s]["h"], tc.slot[s].h, 0, 23);
      getNum(sa[s]["m"], tc.slot[s].m, 0, 59);
      getNum(sa[s]["days"], tc.slot[s].days, 0, 0x7F);
      getStr(sa[s]["note"], tc.slot[s].note, 40);
    }
  }
  return true;
}

// ---------------- migration from the original firmware (v1.x NVS) ----------------
static void migrateLegacy() {
  Preferences p;
  bool any = false;
  if (p.begin("wifi", true)) {
    if (p.isKey("ssid")) { cfg.ssid = p.getString("ssid", ""); cfg.pass = p.getString("password", ""); any = true; }
    p.end();
  }
  if (p.begin("appSettings", true)) {
    if (p.isKey("installedTrays")) {
      any = true;
      cfg.trays = constrain(p.getInt("installedTrays", 1), 1, MAX_TRAYS);
      cfg.mute = p.getBool("muteSound", false);
      cfg.clockSaver = p.getBool("clockSS", true);
      cfg.volume = constrain(p.getInt("spkvolume", 20), 1, 30);
      int off = p.getInt("utcOffset", 0);
      if (p.getBool("dstEnabled", false)) off += 1;
      // POSIX TZ sign is inverted: UTC+5 -> "UTC-5"
      cfg.tz = off == 0 ? "UTC0" : String("UTC") + (off > 0 ? "-" : "+") + String(abs(off));
      cfg.tzName = String("UTC") + (off >= 0 ? "+" : "") + String(off);
      cfg.hostname = p.getString("mdnshostname", "pilldisp");
      cfg.tgToken = p.getString("bot_token", "");
      cfg.tgPatient = p.getString("chat_id", "");
      cfg.pin = p.getString("passcode", "");
      bool en = p.getBool("enabledpasscode", false);
      cfg.pinDispense = en;
      cfg.pinSettings = en;
      if (!en) cfg.pin = "";
    }
    p.end();
  }
  if (p.begin("traySettings", true)) {
    char k[20];
    for (int t = 1; t <= MAX_TRAYS; t++) {
      snprintf(k, sizeof(k), "trayName%d", t);
      if (!p.isKey(k)) continue;
      any = true;
      TrayCfg& tc = cfg.tray[t];
      tc.name = p.getString(k, "");
      snprintf(k, sizeof(k), "trayHour%d", t);
      tc.slot[0].h = constrain(p.getInt(k, 8), 0, 23);
      snprintf(k, sizeof(k), "trayMin%d", t);
      tc.slot[0].m = constrain(p.getInt(k, 0), 0, 59);
      snprintf(k, sizeof(k), "trayAlertEna%d", t);
      tc.alerts = p.getBool(k, false);
      tc.slot[0].on = tc.alerts;
      snprintf(k, sizeof(k), "trayColor%d", t);
      tc.person = constrain(p.getInt(k, 0), 0, MAX_PEOPLE - 1);
    }
    p.end();
  }
  if (any) Serial.println("Migrated settings from original firmware");
}

// ---------------- config file ----------------
void loadConfig() {
  for (int i = 0; i < MAX_PEOPLE; i++) cfg.people[i] = COLOR_NAMES[i];
  for (int t = 1; t <= MAX_TRAYS; t++) cfg.tray[t].name = String("Tray ") + t;
  String err;
  bool ok = false;
  if (fsOk && LittleFS.exists(CFG_FILE)) {
    File f = LittleFS.open(CFG_FILE, "r");
    String s = f.readString();
    f.close();
    ok = configFromJson(s.c_str(), err);
    if (!ok) Serial.printf("config.json error: %s\n", err.c_str());
  }
  if (ok) return;
  // settings file missing or damaged (e.g. the file system was reformatted): use the copy kept in NVS
  Preferences p;
  String copy;
  if (p.begin("pdcfg", true)) { copy = p.getString("json", ""); p.end(); }
  if (copy.length() && configFromJson(copy.c_str(), err)) {
    Serial.println("Settings restored from the backup copy");
    saveConfig();
    return;
  }
  migrateLegacy();
  saveConfig();
}

void saveConfig() {
  if (!fsOk) return;
  String s;
  configToJson(s, true);
  File f = LittleFS.open("/config.tmp", "w");
  if (!f) return;
  f.print(s);
  f.close();
  LittleFS.remove(CFG_FILE);
  LittleFS.rename("/config.tmp", CFG_FILE);
  // second copy in NVS (a separate, wear-levelled flash area) in case the file system is ever lost
  Preferences p;
  if (p.begin("pdcfg", false)) {
    if (p.getString("json", "") != s) p.putString("json", s);
    p.end();
  }
}

// ---------------- persistent state ----------------
void loadPersist() {
  memset(&ps, 0, sizeof(ps));
  for (int t = 0; t <= MAX_TRAYS; t++) ps.stock[t] = cfg.capacity;
  if (!fsOk || !LittleFS.exists(ST_FILE)) { g_freshState = true; savePersist(); return; }
  File f = LittleFS.open(ST_FILE, "r");
  JsonDocument d;
  if (deserializeJson(d, f)) { f.close(); return; }
  f.close();
  JsonArrayConst tr = d["t"];
  for (int t = 1; t <= MAX_TRAYS && t - 1 < (int)tr.size(); t++) {
    JsonObjectConst o = tr[t - 1];
    ps.stock[t] = o["s"] | (int)cfg.capacity;
    ps.lastDisp[t] = o["d"] | 0;
    ps.lowNotified[t] = o["l"] | false;
    ps.lastEv[t] = o["e"] | 0;
    ps.lastEvTime[t] = o["et"] | 0;
    JsonArrayConst se = o["se"];  // per-slot outcome: [ev, yyyymmdd] ...
    for (int s = 0; s < MAX_SLOTS && s < (int)se.size(); s++) {
      ps.slotEv[t][s] = se[s][0] | 0;
      ps.slotEvDay[t][s] = se[s][1] | 0;
    }
    JsonArrayConst sd = o["sd"];
    for (int s = 0; s < MAX_SLOTS && s < (int)sd.size(); s++) ps.slotDay[t][s] = sd[s];
    JsonArrayConst a = o["a"];  // dose that was live when the device went down
    if (a.size() == 4) {
      uint8_t st = a[0];
      act[t].st = st == DS_DUE ? DS_DUE : DS_READY;  // interrupted dispensing: pills may be in the cup
      act[t].slot = a[1];
      act[t].due = a[2];
      act[t].snoozeUntil = a[3];
      act[t].dispensedAt = act[t].st == DS_READY ? (uint32_t)a[2] : 0;
    }
  }
  ps.lastDailyDay = d["dd"] | 0;
  JsonArrayConst dt = d["dt"];
  for (int i = 0; i < 8 && i < (int)dt.size(); i++) ps.dispTimes[i] = dt[i];
  ps.lastInteract = d["li"] | 0;
  ps.checkinDay = d["cd"] | 0;
}

void savePersist() {
  psDirty = false;
  if (!fsOk) return;
  JsonDocument d;
  JsonArray tr = d["t"].to<JsonArray>();
  for (int t = 1; t <= MAX_TRAYS; t++) {
    JsonObject o = tr.add<JsonObject>();
    o["s"] = ps.stock[t];
    o["d"] = ps.lastDisp[t];
    o["l"] = ps.lowNotified[t];
    o["e"] = ps.lastEv[t];
    o["et"] = ps.lastEvTime[t];
    JsonArray se = o["se"].to<JsonArray>();
    for (int s = 0; s < MAX_SLOTS; s++) {
      JsonArray x = se.add<JsonArray>();
      x.add(ps.slotEv[t][s]);
      x.add(ps.slotEvDay[t][s]);
    }
    JsonArray sd = o["sd"].to<JsonArray>();
    for (int s = 0; s < MAX_SLOTS; s++) sd.add(ps.slotDay[t][s]);
    if (act[t].st != DS_NONE) {
      JsonArray a = o["a"].to<JsonArray>();
      a.add((uint8_t)act[t].st);
      a.add(act[t].slot);
      a.add(act[t].st == DS_DUE ? act[t].due : act[t].dispensedAt);
      a.add(act[t].snoozeUntil);
    }
  }
  d["dd"] = ps.lastDailyDay;
  JsonArray dt = d["dt"].to<JsonArray>();
  for (int i = 0; i < 8; i++) dt.add(ps.dispTimes[i]);
  d["li"] = ps.lastInteract;
  d["cd"] = ps.checkinDay;
  File f = LittleFS.open(ST_FILE, "w");
  if (!f) return;
  serializeJson(d, f);
  f.close();
}

void persistDirty() {
  if (!psDirty) psDirtyAt = millis();
  psDirty = true;
}

void storageTick() {
  if (psDirty && millis() - psDirtyAt > 2000) savePersist();
}

// ---------------- history ----------------
void logEvent(uint8_t ev, uint8_t tray, uint8_t slot) {
  uint32_t now = time(nullptr);
  if (tray >= 1 && tray <= MAX_TRAYS && ev != EV_DUE && ev != EV_SNOOZED && ev != EV_ESCALATED) {
    ps.lastEv[tray] = ev;
    ps.lastEvTime[tray] = now;
    bool outcome = ev == EV_DISPENSED || ev == EV_TAKEN || ev == EV_EARLY || ev == EV_SKIPPED || ev == EV_MISSED ||
                   ev == EV_UNCONFIRMED;
    if (outcome && slot < MAX_SLOTS && timeValid()) {
      time_t n = now;
      struct tm lt;
      localtime_r(&n, &lt);
      ps.slotEv[tray][slot] = ev;
      ps.slotEvDay[tray][slot] = dayKey(lt);
    }
    persistDirty();
  }
  Serial.printf("[event] %s tray=%d slot=%d\n", eventName(ev), tray, slot);
  if (!fsOk || !timeValid()) return;
  LogRec r{now, ev, tray, slot, (uint8_t)(tray >= 1 && tray <= MAX_TRAYS ? cfg.tray[tray].person : 0xFF)};
  File f = LittleFS.open(LOG_FILE, "a");
  if (!f) return;
  f.write((const uint8_t*)&r, sizeof(r));
  size_t sz = f.size();
  f.close();
  if (sz >= LOG_MAX) {
    LittleFS.remove(LOG_OLD);
    LittleFS.rename(LOG_FILE, LOG_OLD);
  }
}

static void appendFile(const char* path, String& out, uint32_t since, bool& first) {
  if (!LittleFS.exists(path)) return;
  File f = LittleFS.open(path, "r");
  LogRec r;
  char buf[48];
  while (f.read((uint8_t*)&r, sizeof(r)) == sizeof(r)) {
    if (r.t < since) continue;
    snprintf(buf, sizeof(buf), "%s[%lu,%u,%u,%u,%u]", first ? "" : ",", (unsigned long)r.t, r.ev, r.tray,
             r.slot == 0xFF ? 9 : r.slot, r.person == 0xFF ? 9 : r.person);
    out += buf;
    first = false;
  }
  f.close();
}

void historyJson(String& out, uint32_t since) {
  out.reserve(16000);
  out = "[";
  bool first = true;
  if (fsOk) {
    appendFile(LOG_OLD, out, since, first);
    appendFile(LOG_FILE, out, since, first);
  }
  out += "]";
}

void factoryReset() {
  if (fsOk) {
    LittleFS.remove(CFG_FILE);
    LittleFS.remove(ST_FILE);
    LittleFS.remove(LOG_FILE);
    LittleFS.remove(LOG_OLD);
  }
  Preferences p;
  const char* ns[] = {"wifi", "appSettings", "traySettings"};
  for (auto n : ns) if (p.begin(n, false)) { p.clear(); p.end(); }
}
