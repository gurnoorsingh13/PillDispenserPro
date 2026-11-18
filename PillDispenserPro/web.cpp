// Web portal: single-page app (gzipped in flash), JSON API, OTA, legacy Home Assistant endpoints
#include "app.h"
#include <WiFi.h>
#include <Update.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include "web_page.h"

static AsyncWebServer server(80);

static bool auth(AsyncWebServerRequest* r) {
  String pass;
  { Lock l; pass = cfg.webPass; }
  if (!pass.length() || r->authenticate("admin", pass.c_str())) return true;
  r->requestAuthentication("PillDispenser");
  return false;
}

static int intParam(AsyncWebServerRequest* r, const char* name, int def = 0) {
  if (r->hasParam(name)) return r->getParam(name)->value().toInt();
  if (r->hasParam(name, true)) return r->getParam(name, true)->value().toInt();
  return def;
}
static String strParam(AsyncWebServerRequest* r, const char* name) {
  if (r->hasParam(name, true)) return r->getParam(name, true)->value();
  if (r->hasParam(name)) return r->getParam(name)->value();
  return "";
}

static void sendJson(AsyncWebServerRequest* r, int code, const String& body) {
  AsyncWebServerResponse* res = r->beginResponse(code, "application/json", body);
  res->addHeader("Cache-Control", "no-store");
  r->send(res);
}
static void ok(AsyncWebServerRequest* r, const char* msg = "ok") { sendJson(r, 200, String("{\"ok\":true,\"msg\":\"") + msg + "\"}"); }
static void fail(AsyncWebServerRequest* r, int code, const String& msg) {
  String m = msg;
  m.replace("\"", "'");
  sendJson(r, code, String("{\"ok\":false,\"msg\":\"") + m + "\"}");
}

// collects a request body into _tempObject (freed by the library)
static void collectBody(AsyncWebServerRequest* r, uint8_t* data, size_t len, size_t index, size_t total) {
  if (total > 24000) return;
  if (index == 0) {
    r->_tempObject = malloc(total + 1);
    if (!r->_tempObject) return;
  }
  if (!r->_tempObject) return;
  memcpy((uint8_t*)r->_tempObject + index, data, len);
  if (index + len == total) ((char*)r->_tempObject)[total] = 0;
}

static const char* modeName(AppMode m) {
  switch (m) {
    case M_ALERT: return "alert";
    case M_SNOOZED: return "snoozed";
    case M_DISPENSING: return "dispensing";
    case M_READY: return "ready";
    default: return "idle";
  }
}

static void statusJson(String& out) {
  JsonDocument d;
  {
    Lock l;
    d["v"] = FW_VERSION;
    d["team"] = TEAM_NAME;
    d["members"] = TEAM_MEMBERS;
    d["host"] = cfg.hostname;
    d["ip"] = localIp();
    d["ssid"] = netConnected() ? WiFi.SSID() : String("");
    d["rssi"] = netConnected() ? WiFi.RSSI() : 0;
    d["ap"] = apActive();
    if (apActive()) d["apSsid"] = apSsid();
    d["timeOk"] = (bool)g_timeOk;
    d["now"] = (uint32_t)time(nullptr);
    d["tzName"] = cfg.tzName;
    d["up"] = millis() / 1000;
    d["heap"] = ESP.getFreeHeap();
    d["mode"] = modeName(appMode());
    d["alertPerson"] = alertPerson();
    d["readyPerson"] = readyPerson;
    d["busy"] = servoBusy();
    d["servoOk"] = servoBoardOk();
    d["trays"] = cfg.trays;
    d["cap"] = cfg.capacity;
    d["tg"] = cfg.tgToken.length() > 0;
    d["safe"] = g_safeMode;
    d["ai"] = cfg.aiKey.length() > 0;
    d["demo"] = demoDueAt();
    d["party"] = g_party;
    d["ntest"] = g_lastTest;
    d["chan"] = (cfg.tgToken.length() ? 1 : 0) | (cfg.ntfyTopic.length() ? 2 : 0) | (cfg.waPhone.length() && cfg.waKey.length() ? 4 : 0) |
                (cfg.callUser.length() ? 8 : 0) | (cfg.hookUrl.length() ? 16 : 0);
    if (g_sos.on) {
      JsonObject so = d["sos"].to<JsonObject>();
      so["at"] = g_sos.at;
      so["ack"] = g_sos.ack;
    }
    if (g_msg.at) {
      JsonObject mo = d["msg"].to<JsonObject>();
      mo["text"] = g_msg.text;
      mo["from"] = g_msg.from;
      mo["at"] = g_msg.at;
      mo["read"] = g_msg.readAt;
    }
    uint8_t nt, ns;
    uint32_t when;
    if (nextDose(nt, ns, when)) {
      JsonObject n = d["next"].to<JsonObject>();
      n["tray"] = nt;
      n["slot"] = ns;
      n["t"] = when;
    }
    JsonArray pa = d["people"].to<JsonArray>();
    for (int i = 0; i < MAX_PEOPLE; i++) pa.add(cfg.people[i]);
    JsonArray ta = d["tray"].to<JsonArray>();
    for (int t = 1; t <= MAX_TRAYS; t++) {
      JsonObject o = ta.add<JsonObject>();
      o["name"] = cfg.tray[t].name;
      o["person"] = cfg.tray[t].person;
      o["alerts"] = cfg.tray[t].alerts;
      o["stock"] = ps.stock[t];
      o["days"] = daysLeft(t);
      o["last"] = ps.lastDisp[t];
      o["ev"] = ps.lastEv[t];
      o["evT"] = ps.lastEvTime[t];
      o["sched"] = scheduleText(t);
      o["st"] = (uint8_t)act[t].st;
      o["slot"] = act[t].slot;
      o["due"] = act[t].due;
      o["snooze"] = act[t].snoozeUntil;
    }
  }
  serializeJson(d, out);
}

static void handleAction(AsyncWebServerRequest* r) {
  if (!auth(r)) return;
  String cmd = strParam(r, "cmd");
  int tray = intParam(r, "tray");
  int n = intParam(r, "n");
  bool force = intParam(r, "force") == 1;
  uint8_t src = SRC_WEB;
  if ((cmd == "dispense" || cmd == "due" || cmd == "advance" || cmd == "align") && !servoBoardOk())
    return fail(r, 503, "Servo board not found - check base PCB cable and power");
  if (cmd == "dispense") {
    const char* why;
    { Lock l; why = manualCheck(tray); }
    if (why && !force) return fail(r, 409, why);
    postCmd(C_MANUAL, src, tray, force);
  } else if (cmd == "due") postCmd(C_DISPENSE_DUE, src, -1);
  else if (cmd == "taken") postCmd(C_TAKEN, src, -1);
  else if (cmd == "snooze") postCmd(C_SNOOZE, src);
  else if (cmd == "skip") postCmd(C_SKIP, src, -1);
  else if (cmd == "advance") postCmd(C_ADVANCE, src, tray, n ? n : 1);
  else if (cmd == "align") postCmd(C_ALIGN, src, tray);
  else if (cmd == "refill") postCmd(C_REFILL, src, tray, n);
  else if (cmd == "sound") postCmd(C_SOUND, src, n, intParam(r, "voice"));
  else if (cmd == "tgtest") postCmd(C_TG_TEST, src);
  else if (cmd == "identify") postCmd(C_IDENTIFY, src);
  else if (cmd == "sos") postCmd(C_SOS, src);
  else if (cmd == "sos_clear") postCmd(C_SOS_CANCEL, src, 0, 0, "the web portal");
  else if (cmd == "ntest") postCmd(C_NTEST, src, n);
  else if (cmd == "demo") postCmd(C_DEMO, src, n, force);
  else if (cmd == "party") postCmd(C_PARTY, src, n);
  else if (cmd == "msg") {
    String text = strParam(r, "text"), from = strParam(r, "from");
    text.trim();
    if (!text.length()) return fail(r, 400, "Type a message first");
    postCmd(C_MSG, src, 0, 0, (String("\n") + (from.length() ? from : String("Family")) + "\n" + text).c_str());
  }
  else if (cmd == "settime") postCmd(C_SETTIME, src, intParam(r, "t"), force);
  else if (cmd == "reboot") postCmd(C_REBOOT, src);
  else if (cmd == "factory") postCmd(C_FACTORY, src);
  else return fail(r, 400, "unknown command");
  ok(r);
}

// ---------------- legacy v1.x endpoints (Home Assistant integration) ----------------
static bool legacyTray(AsyncWebServerRequest* r, const char* param, int& t) {
  if (!r->hasParam(param)) { r->send(400, "text/plain", String("Param '") + param + "' is missing."); return false; }
  t = r->getParam(param)->value().toInt();
  if (t < 1 || t > cfg.trays) { r->send(400, "text/plain", "Wrong value for tray or tray not installed."); return false; }
  return true;
}

static bool evToday(uint8_t t, bool good) {
  if (!ps.lastEvTime[t] || !g_timeOk) return false;
  time_t now = time(nullptr), e = ps.lastEvTime[t];
  struct tm a, b;
  localtime_r(&now, &a);
  localtime_r(&e, &b);
  if (dayKey(a) != dayKey(b)) return false;
  uint8_t ev = ps.lastEv[t];
  return good ? (ev == EV_DISPENSED || ev == EV_TAKEN || ev == EV_EARLY || ev == EV_MANUAL)
              : (ev == EV_SKIPPED || ev == EV_MISSED);
}

static void legacyRoutes() {
  server.on("/dispense", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!auth(r)) return;
    bool due;
    { Lock l; due = appMode() == M_ALERT || appMode() == M_SNOOZED; }
    if (!due) return r->send(400, "text/plain", "No alarm in progress. Nothing to dispense.");
    postCmd(C_DISPENSE_DUE, SRC_API, -1);
    r->send(200, "text/plain", "Done");
  });
  server.on("/dismiss", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!auth(r)) return;
    postCmd(C_SKIP, SRC_API, -1);
    r->send(200, "text/plain", "Done");
  });
  server.on("/dropout", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!auth(r)) return;
    int t;
    if (!legacyTray(r, "tray", t)) return;
    postCmd(C_MANUAL, SRC_API, t, 1);
    r->send(200, "text/plain", "Done");
  });
  server.on("/fix", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!auth(r)) return;
    int t;
    if (!legacyTray(r, "tray", t)) return;
    postCmd(C_ALIGN, SRC_API, t);
    r->send(200, "text/plain", "Done");
  });
  server.on("/alarm", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!auth(r)) return;
    postCmd(C_SOUND, SRC_API, r->hasParam("id") ? r->getParam("id")->value().toInt() : TRACK_READY);
    r->send(200, "text/plain", "Done");
  });
  struct Q { const char* path; uint8_t kind; };
  static const Q qs[] = {{"/tr_name", 0}, {"/tr_color", 1}, {"/tr_inst", 2}, {"/tr_enab", 3},
                         {"/tr_trig", 4}, {"/tr_disp", 5}, {"/tr_dism", 6}, {"/tr_time", 7}};
  for (const Q& q : qs) {
    uint8_t kind = q.kind;
    server.on(q.path, HTTP_GET, [kind](AsyncWebServerRequest* r) {
      if (!auth(r)) return;
      int t;
      Lock l;
      if (!legacyTray(r, "id", t)) return;
      String v;
      switch (kind) {
        case 0: v = cfg.tray[t].name; break;
        case 1: v = String(cfg.tray[t].person); break;
        case 2: v = "1"; break;
        case 3: v = cfg.tray[t].alerts ? "1" : "0"; break;
        case 4: v = act[t].st == DS_DUE ? "1" : "0"; break;
        case 5: v = evToday(t, true) ? "1" : "0"; break;
        case 6: v = evToday(t, false) ? "1" : "0"; break;
        case 7: {
          const DoseSlot* s = nullptr;
          for (auto& sl : cfg.tray[t].slot) if (sl.on) { s = &sl; break; }
          if (!cfg.tray[t].alerts || !s) return r->send(400, "text/plain", "This tray is not enabled for daily alerts");
          v = String(s->h) + ":" + String(s->m);
        }
      }
      r->send(200, "text/plain", v);
    });
  }
  server.on("/IP", HTTP_GET, [](AsyncWebServerRequest* r) { r->send(200, "text/plain", localIp()); });
  server.on("/AP", HTTP_GET, [](AsyncWebServerRequest* r) { r->send(200, "text/plain", WiFi.SSID()); });
  server.on("/DB", HTTP_GET, [](AsyncWebServerRequest* r) { r->send(200, "text/plain", String(WiFi.RSSI())); });
}

// ---------------- routes ----------------
static void sendPage(AsyncWebServerRequest* r) {
  if (!auth(r)) return;
  AsyncWebServerResponse* res = r->beginResponse_P(200, "text/html", INDEX_GZ, INDEX_GZ_LEN);
  res->addHeader("Content-Encoding", "gzip");
  res->addHeader("Cache-Control", "no-cache");
  r->send(res);
}

void webInit() {
  server.on("/", HTTP_GET, sendPage);
  server.on("/update", HTTP_GET, sendPage);

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!auth(r)) return;
    String s;
    statusJson(s);
    sendJson(r, 200, s);
  });

  server.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!auth(r)) return;
    String s;
    { Lock l; configToJson(s, false); }
    sendJson(r, 200, s);
  });

  // the web app calls the AI from the phone (photos never pass through the ESP32): it needs the real key
  server.on("/api/ai", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!auth(r)) return;
    JsonDocument d;
    { Lock l; d["key"] = cfg.aiKey; d["model"] = cfg.aiModel; d["vision"] = cfg.aiVision; }
    String s;
    serializeJson(d, s);
    sendJson(r, 200, s);
  });

  server.on("/api/backup", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!auth(r)) return;
    String s;
    { Lock l; configToJson(s, true); }
    AsyncWebServerResponse* res = r->beginResponse(200, "application/json", s);
    res->addHeader("Content-Disposition", "attachment; filename=pilldispenser-backup.json");
    r->send(res);
  });

  auto bodyRoute = [](const char* path, uint8_t cmd) {
    server.on(path, HTTP_POST, [cmd](AsyncWebServerRequest* r) {
      if (!auth(r)) return;
      if (!r->_tempObject) return fail(r, 400, "empty body");
      JsonDocument probe;
      if (deserializeJson(probe, (const char*)r->_tempObject)) return fail(r, 400, "invalid JSON");
      postCmd(cmd, SRC_WEB, 0, 0, (const char*)r->_tempObject);
      ok(r, "saved");
    }, nullptr, collectBody);
  };
  bodyRoute("/api/config", C_CONFIG);
  bodyRoute("/api/restore", C_RESTORE);

  server.on("/api/action", HTTP_POST, handleAction);
  server.on("/api/action", HTTP_GET, handleAction);

  server.on("/api/history", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!auth(r)) return;
    int days = constrain(intParam(r, "days", 31), 1, 400);
    uint32_t since = (uint32_t)time(nullptr) - days * 86400UL;
    String s;
    { Lock l; historyJson(s, since); }
    sendJson(r, 200, s);
  });

  server.on("/api/wifi/scan", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!auth(r)) return;
    String s;
    int n = scanResults(s);
    if (n < 0) return sendJson(r, 202, "{\"scanning\":true}");
    sendJson(r, 200, s);
  });

  server.on("/api/wifi", HTTP_POST, [](AsyncWebServerRequest* r) {
    if (!auth(r)) return;
    String ssid = strParam(r, "ssid"), pass = strParam(r, "pass");
    if (!ssid.length()) return fail(r, 400, "SSID required");
    postCmd(C_WIFI, SRC_WEB, 0, 0, (ssid + "\n" + pass).c_str());
    ok(r, "connecting");
  });

  server.on("/api/update", HTTP_POST,
    [](AsyncWebServerRequest* r) {
      if (!auth(r)) return;
      bool good = !Update.hasError();
      if (good) { ok(r, "updated - rebooting"); postCmd(C_REBOOT); }
      else fail(r, 500, String("update failed: ") + Update.errorString());
    },
    [](AsyncWebServerRequest* r, const String& fn, size_t index, uint8_t* data, size_t len, bool final) {
      String pass;
      { Lock l; pass = cfg.webPass; }
      if (pass.length() && !r->authenticate("admin", pass.c_str())) return;
      if (index == 0) {
        Serial.printf("OTA start: %s\n", fn.c_str());
        uiToast(LV_SYMBOL_DOWNLOAD " Updating firmware...");
        if (!Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH)) Update.printError(Serial);
      }
      if (len && Update.write(data, len) != len) Update.printError(Serial);
      if (final && !Update.end(true)) Update.printError(Serial);
    });

  legacyRoutes();

  server.onNotFound([](AsyncWebServerRequest* r) {
    // captive portal: any host while the setup hotspot is up goes to the setup page
    if (apActive() && r->host() != WiFi.softAPIP().toString()) {
      r->redirect(String("http://") + WiFi.softAPIP().toString() + "/");
      return;
    }
    r->send(404, "text/plain", "Not found");
  });

  DefaultHeaders::Instance().addHeader("X-Device", "PillDispenser");
  server.begin();
}
