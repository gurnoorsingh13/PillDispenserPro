// Notification hub in its own task (HTTPS calls block for seconds; the UI must never freeze).
// Channels: Telegram (two-way), ntfy push, WhatsApp (CallMeBot), voice call (CallMeBot), webhook.
// Only one TLS connection is open at a time: the ESP32 has no PSRAM.
#include "app.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>

struct Out { char* chat; char* text; uint8_t target, level, kb, only; bool test; };
static QueueHandle_t outQ;
struct AiJob { char* chat; char* body; };
static QueueHandle_t aiQ;

bool aiQueue(const String& chat, const String& body) {
  if (!aiQ) return false;
  AiJob j{strdup(chat.c_str()), strdup(body.c_str())};
  if (xQueueSend(aiQ, &j, 0) != pdTRUE) { free(j.chat); free(j.body); return false; }
  return true;
}

// Groq chat completions (OpenAI-compatible). Returns the answer, or a short explanation of what went wrong.
static String aiCall(const String& key, const char* body) {
  WiFiClientSecure tls;
  tls.setInsecure();
  HTTPClient http;
  http.setTimeout(30000);
  if (!http.begin(tls, "https://api.groq.com/openai/v1/chat/completions")) return "I could not reach the AI service.";
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", "Bearer " + key);
  int code = http.POST((uint8_t*)body, strlen(body));
  String resp = code > 0 ? http.getString() : String("");
  http.end();
  if (code <= 0) return "I could not reach the AI service (" + String(code) + "). Is the dispenser online?";
  JsonDocument filter;
  filter["choices"][0]["message"]["content"] = true;
  filter["error"]["message"] = true;
  JsonDocument d;
  if (deserializeJson(d, resp, DeserializationOption::Filter(filter))) return "The AI service sent an unreadable reply (HTTP " + String(code) + ").";
  String a = d["choices"][0]["message"]["content"] | "";
  a.trim();
  if (a.length()) return a;
  String err = d["error"]["message"] | "";
  if (code == 401) return "The AI key was refused. Check it in the web app: Settings - AI assistant.";
  if (code == 429) return "The free AI limit is used up for now. Please try again in a minute.";
  return "AI error (HTTP " + String(code) + ")" + (err.length() ? ": " + err : String(""));
}
String g_lastTest;

static const char* KB_DOSE_JSON =
    "[[{\"text\":\"💊 Dispense\",\"callback_data\":\"/dispense\"},"
    "{\"text\":\"⏰ Snooze\",\"callback_data\":\"/snooze\"}],"
    "[{\"text\":\"✅ Taken\",\"callback_data\":\"/taken\"},"
    "{\"text\":\"📋 Status\",\"callback_data\":\"/status\"}]]";
static const char* KB_SOS_JSON =
    "[[{\"text\":\"🏃 I'm on my way\",\"callback_data\":\"/coming\"}],"
    "[{\"text\":\"✅ Handled - clear SOS\",\"callback_data\":\"/ok\"}]]";

static bool anyChannel() {
  return cfg.tgToken.length() || cfg.ntfyTopic.length() || (cfg.waPhone.length() && cfg.waKey.length()) ||
         cfg.callUser.length() || cfg.hookUrl.length();
}

static bool push(const char* chat, const String& text, uint8_t target, uint8_t level, uint8_t kb, uint8_t only, bool test = false) {
  if (!outQ) return false;
  { Lock l; if (!anyChannel()) return false; }
  Out o{chat ? strdup(chat) : nullptr, strdup(text.c_str()), target, level, kb, only, test};
  if (xQueueSend(outQ, &o, 0) != pdTRUE) { free(o.chat); free(o.text); return false; }
  return true;
}

void notify(uint8_t target, const String& text, uint8_t level, uint8_t kb) { push(nullptr, text, target, level, kb, CH_ALL); }
void tgSend(uint8_t target, const String& text, bool kb) { notify(target, text, NL_INFO, kb ? KB_DOSE : KB_NONE); }
void tgSendTo(const String& chat, const String& text, bool kb) { push(chat.c_str(), text, 0, NL_INFO, kb ? KB_DOSE : KB_NONE, CH_TELEGRAM); }

static const char* const CH_NAMES[] = {"All", "Telegram", "Push (ntfy)", "WhatsApp", "Voice call", "Webhook"};
void notifyTest(uint8_t ch) {
  if (ch < CH_TELEGRAM || ch > CH_WEBHOOK) return;
  g_lastTest = String(CH_NAMES[ch]) + ": sending...";
  String who;
  { Lock l; who = cfg.people[cfg.tray[1].person]; }
  if (!push(nullptr, String("👋 Test from your PillDispenser (") + CH_NAMES[ch] + "). Alerts for " + who + " will arrive here.",
            TG_ALL, ch == CH_CALL ? NL_URGENT : NL_IMPORTANT, KB_NONE, ch, true))
    g_lastTest = String(CH_NAMES[ch]) + ": not set up - save the settings first";
}

// ---------------- Telegram recipients ----------------
static void splitIds(const String& list, String* out, int& n, int max) {
  int start = 0;
  while (start <= (int)list.length() && n < max) {
    int c = list.indexOf(',', start);
    if (c < 0) c = list.length();
    String id = list.substring(start, c);
    id.trim();
    if (id.length()) out[n++] = id;
    start = c + 1;
  }
}

bool tgAuthorized(const String& chat) {
  Lock l;
  String ids[10];
  int n = 0;
  splitIds(cfg.tgPatient, ids, n, 5);
  splitIds(cfg.tgCare, ids, n, 10);
  for (int i = 0; i < n; i++) if (ids[i] == chat) return true;
  return false;
}

static void recipients(uint8_t target, String* ids, int& n) {
  Lock l;
  n = 0;
  bool hasP = cfg.tgPatient.length(), hasC = cfg.tgCare.length();
  // fall back to whichever list exists so nothing is silently dropped
  if ((target & TG_PATIENT) || ((target & TG_CARE) && !hasC)) splitIds(cfg.tgPatient, ids, n, 5);
  if ((target & TG_CARE) || ((target & TG_PATIENT) && !hasP)) splitIds(cfg.tgCare, ids, n, 10);
  for (int i = 0; i < n; i++)  // de-duplicate
    for (int j = i + 1; j < n; j++)
      if (ids[i] == ids[j]) { ids[j] = ids[--n]; j--; }
}

// ---------------- HTTP helpers ----------------
static String urlEncode(const String& s) {
  String o;
  char b[4];
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { snprintf(b, sizeof(b), "%%%02X", c); o += b; }
  }
  return o;
}

static String ascii(const String& s) {  // text-to-speech: drop emoji and other non-ASCII
  String o;
  for (unsigned char c : s) if (c >= 32 && c < 127) o += (char)c;
  o.trim();
  return o;
}

static String jsonEsc(const String& s) {
  String o;
  for (char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n') o += "\\n";
    else if ((uint8_t)c >= 32 || c < 0) o += c;
  }
  return o;
}

// one request, returns HTTP status (or a negative HTTPClient error)
static int httpCall(const String& url, const char* method, const String& body, const char* ctype,
                    const char* h1 = nullptr, const String& v1 = "", const char* h2 = nullptr, const String& v2 = "") {
  WiFiClientSecure tls;
  WiFiClient plain;
  tls.setInsecure();  // third-party APIs: no certificate pinning on a device without updates
  HTTPClient http;
  http.setTimeout(9000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  bool ok = url.startsWith("https") ? http.begin(tls, url) : http.begin(plain, url);
  if (!ok) return -100;
  if (ctype) http.addHeader("Content-Type", ctype);
  if (h1) http.addHeader(h1, v1);
  if (h2) http.addHeader(h2, v2);
  int code = !strcmp(method, "POST") ? http.POST(body) : http.GET();
  http.end();
  return code;
}

static int sendNtfy(const String& topic, const String& text, uint8_t level) {
  String url = topic.startsWith("http") ? topic : "https://ntfy.sh/" + topic;
  const char* prio = level >= NL_URGENT ? "5" : level == NL_IMPORTANT ? "4" : "3";
  return httpCall(url, "POST", text, "text/plain; charset=utf-8", "Title", "PillDispenser", "Priority", prio);
}

static int sendWhatsApp(const String& phone, const String& key, const String& text) {
  return httpCall("https://api.callmebot.com/whatsapp.php?phone=" + urlEncode(phone) + "&text=" + urlEncode(text) +
                      "&apikey=" + urlEncode(key), "GET", "", nullptr);
}

static int sendCall(const String& user, const String& text) {
  String t = ascii(text);
  if (t.length() > 220) t = t.substring(0, 220);
  return httpCall("https://api.callmebot.com/start.php?source=web&user=" + urlEncode(user) + "&text=" + urlEncode(t) +
                      "&lang=en-GB-Standard-B&rpt=2", "GET", "", nullptr);
}

static int sendHook(const String& url, const String& host, const Out& o) {
  String body = String("{\"device\":\"") + jsonEsc(host) + "\",\"time\":" + (uint32_t)time(nullptr) +
                ",\"level\":" + o.level + ",\"to\":\"" + (o.target == TG_PATIENT ? "patient" : o.target == TG_CARE ? "caregivers" : "all") +
                "\",\"text\":\"" + jsonEsc(o.text) + "\"}";
  return httpCall(url, "POST", body, "application/json");
}

// ---------------- clock fallback: HTTP "Date" header when NTP is blocked ----------------
static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static void httpDateSync() {
  WiFiClient c;
  if (!c.connect("connectivitycheck.gstatic.com", 80, 4000)) return;
  c.print("HEAD /generate_204 HTTP/1.1\r\nHost: connectivitycheck.gstatic.com\r\nConnection: close\r\n\r\n");
  uint32_t until = millis() + 4000;
  while (millis() < until && (c.connected() || c.available())) {
    String line = c.readStringUntil('\n');
    if (!line.startsWith("Date:") && !line.startsWith("date:")) continue;
    int d, y, H, M, S;
    char mon[4];
    if (sscanf(line.c_str() + 5, " %*3s, %d %3s %d %d:%d:%d", &d, mon, &y, &H, &M, &S) != 6) break;
    const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char* p = strstr(months, mon);
    if (!p) break;
    int64_t epoch = daysFromCivil(y, (p - months) / 3 + 1, d) * 86400 + H * 3600 + M * 60 + S;
    Serial.printf("Clock from HTTP Date header: %s", line.c_str() + 5);
    postCmd(C_SETTIME, SRC_API, (int32_t)epoch, 0);
    break;
  }
  c.stop();
}

// ---------------- task ----------------
static void cloudTask(void*) {
  WiFiClientSecure tgClient;
  tgClient.setCACert(TELEGRAM_CERTIFICATE_ROOT);
  UniversalTelegramBot* bot = nullptr;
  String token;
  uint32_t lastPoll = 0, connSince = 0, lastDateTry = 0;
  bool drained = false;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(150));
    if (!netConnected()) { connSince = 0; continue; }
    if (!connSince) connSince = millis();
    if (!timeValid() && millis() - connSince > 15000 && millis() - lastDateTry > 30000) {
      lastDateTry = millis();
      httpDateSync();
    }
    if (!g_timeOk) continue;  // TLS certificates need a valid clock

    String tok, ntfy, waPhone, waKey, callUser, hook, host;
    {
      Lock l;
      tok = cfg.tgToken; ntfy = cfg.ntfyTopic; waPhone = cfg.waPhone; waKey = cfg.waKey;
      callUser = cfg.callUser; hook = cfg.hookUrl; host = cfg.hostname;
    }
    if (tok != token) { delete bot; bot = nullptr; token = tok; drained = false; }
    if (tok.length() && !bot) {
      bot = new UniversalTelegramBot(tok, tgClient);
      bot->waitForResponse = 2500;
    }
    if (bot && !drained) {  // ignore commands that were sent while the device was off
      for (int i = 0; i < 10 && bot->getUpdates(bot->last_message_received + 1) > 0; i++) {}
      drained = true;
    }

    Out o;
    while (xQueueReceive(outQ, &o, 0) == pdTRUE) {
      String text(o.text);
      int rc = 0;
      auto want = [&](uint8_t ch) { return o.only == CH_ALL || o.only == ch; };
      if (bot && want(CH_TELEGRAM)) {
        String ids[10];
        int n = 0;
        if (o.chat) { ids[0] = o.chat; n = 1; }
        else recipients(o.target, ids, n);
        const char* kb = o.kb == KB_SOS ? KB_SOS_JSON : o.kb == KB_DOSE ? KB_DOSE_JSON : nullptr;
        bool sent = false;
        for (int i = 0; i < n; i++) sent |= kb ? bot->sendMessageWithInlineKeyboard(ids[i], text, "", kb) : bot->sendMessage(ids[i], text, "");
        Serial.printf("telegram: send to %d chat(s) %s\n", n, sent ? "OK" : "FAILED");
        if (o.test) g_lastTest = n ? String("Telegram: ") + (sent ? "sent" : "failed - check the token") : "Telegram: no chat ids set";
      }
      if (o.test && o.only == CH_TELEGRAM && !bot) g_lastTest = "Telegram: no bot token - save the settings first";
      if (!o.chat) {
        bool other = (ntfy.length() && want(CH_NTFY)) || (waPhone.length() && want(CH_WHATSAPP)) ||
                     (callUser.length() && want(CH_CALL)) || (hook.length() && want(CH_WEBHOOK));
        if (other) tgClient.stop();  // free the Telegram TLS session first (RAM)
        if (ntfy.length() && want(CH_NTFY)) {
          rc = sendNtfy(ntfy, text, o.level);
          if (o.test) g_lastTest = String("Push: ") + (rc == 200 ? "sent" : "failed (" + String(rc) + ")");
        }
        if (waPhone.length() && waKey.length() && want(CH_WHATSAPP) && (o.test || (o.target & TG_CARE))) {
          rc = sendWhatsApp(waPhone, waKey, text);
          if (o.test) g_lastTest = String("WhatsApp: ") + (rc == 200 ? "sent" : "failed (" + String(rc) + ")");
        }
        if (callUser.length() && want(CH_CALL) && (o.test || o.level >= NL_URGENT)) {
          rc = sendCall(callUser, text);
          if (o.test) g_lastTest = String("Voice call: ") + (rc == 200 ? "calling " + callUser : "failed (" + String(rc) + ")");
        }
        if (hook.length() && want(CH_WEBHOOK)) {
          rc = sendHook(hook, host, o);
          if (o.test) g_lastTest = String("Webhook: ") + (rc >= 200 && rc < 300 ? "sent" : "failed (" + String(rc) + ")");
        }
        if (o.test && o.only != CH_TELEGRAM) {
          bool configured = (o.only == CH_NTFY && ntfy.length()) || (o.only == CH_WHATSAPP && waPhone.length() && waKey.length()) ||
                            (o.only == CH_CALL && callUser.length()) || (o.only == CH_WEBHOOK && hook.length());
          if (!configured) g_lastTest = String(CH_NAMES[o.only]) + ": not set up - save the settings first";
        }
      }
      if (o.test) uiToast(g_lastTest);
      free(o.chat);
      free(o.text);
    }

    AiJob j;
    if (xQueueReceive(aiQ, &j, 0) == pdTRUE) {
      String key;
      { Lock l; key = cfg.aiKey; }
      if (bot) bot->sendChatAction(j.chat, "typing");
      tgClient.stop();  // one TLS session at a time
      String ans = key.length() ? aiCall(key, j.body) : String("The AI assistant is not set up.");
      if (ans.length() > 3500) ans = ans.substring(0, 3500);
      tgSendTo(String(j.chat), "🤖 " + ans);
      free(j.chat);
      free(j.body);
    }

    if (bot && millis() - lastPoll > 3000) {
      lastPoll = millis();
      int n = bot->getUpdates(bot->last_message_received + 1);
      for (int i = 0; i < n; i++) {
        telegramMessage& m = bot->messages[i];
        if (m.type == "callback_query") bot->answerCallbackQuery(m.query_id, "OK");
        long age = (long)time(nullptr) - m.date.toInt();
        if (m.type != "callback_query" && age > 300) continue;  // stale
        String line = m.chat_id + "\n" + m.from_name + "\n" + m.text;
        Serial.printf("telegram: from %s (chat %s): %s\n", m.from_name.c_str(), m.chat_id.c_str(), m.text.c_str());
        postCmd(C_TG_CMD, SRC_TELEGRAM, 0, 0, line.c_str());
      }
    }
  }
}

void tgInit() {
  outQ = xQueueCreate(24, sizeof(Out));
  aiQ = xQueueCreate(3, sizeof(AiJob));
  xTaskCreatePinnedToCore(cloudTask, "cloud", 16384, nullptr, 1, nullptr, 0);
}
