// Dose engine: schedules, alerts, dispensing, confirmation, escalation, stock, commands
#include "app.h"
#include <WiFi.h>
#include <sys/time.h>
#include <ArduinoJson.h>

Config cfg;
Persist ps;
ActiveDose act[MAX_TRAYS + 1];
SemaphoreHandle_t g_lock;
int8_t readyPerson = -1;
bool readyIsManual = false;
volatile bool g_timeOk = false;
bool g_freshState = false;
bool g_safeMode = false;
SosState g_sos;
FamilyMsg g_msg;
uint8_t g_party = MUSIC_OFF;
static uint32_t partyUntil = 0;
static uint32_t lastSosSound = 0;

const uint32_t PERSON_RGB[MAX_PEOPLE] = {0xFF0000, 0xFF00FF, 0x800080, 0x0000FF,
                                         0x00FFFF, 0x00FF00, 0xFFFF00, 0xFFA500};
const char* const COLOR_NAMES[MAX_PEOPLE] = {"Red", "Magenta", "Violet", "Blue",
                                             "Cyan", "Green", "Yellow", "Orange"};
static const char* const DAY_SHORT[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static QueueHandle_t cmdQ;
static uint32_t lastSoundMs = 0, readyShownAt = 0, rebootAt = 0, identifyUntil = 0;
static bool ringNow = false;
static uint8_t manualKind[MAX_TRAYS + 1], manualSlot[MAX_TRAYS + 1];
static int8_t dispensingPerson = -1;
static bool groupHasScheduled = false;
static String dueBatch;  // newly due trays collected during one tick (one telegram message)

// ---------------- time helpers ----------------
bool timeValid() { return time(nullptr) > 1700000000; }

uint32_t dayKey(const struct tm& t) { return (t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday; }

static uint32_t epochAt(struct tm day, uint8_t h, uint8_t m) {
  day.tm_hour = h;
  day.tm_min = m;
  day.tm_sec = 0;
  day.tm_isdst = -1;
  return mktime(&day);
}

static String hhmm(uint32_t epoch) {
  time_t e = epoch;
  struct tm lt;
  localtime_r(&e, &lt);
  char b[8];
  strftime(b, sizeof(b), "%H:%M", &lt);
  return b;
}

static String trayLabel(uint8_t t) { return cfg.tray[t].name; }  // one tray: the medicine name is enough
static const String& personName(uint8_t p) { return cfg.people[p < MAX_PEOPLE ? p : 0]; }

const char* eventName(uint8_t ev) {
  switch (ev) {
    case EV_DUE: return "due";
    case EV_DISPENSED: return "dispensed";
    case EV_TAKEN: return "taken";
    case EV_SKIPPED: return "skipped";
    case EV_MISSED: return "missed";
    case EV_SNOOZED: return "snoozed";
    case EV_MANUAL: return "manual";
    case EV_EARLY: return "early";
    case EV_REFILL: return "refill";
    case EV_UNCONFIRMED: return "unconfirmed";
    case EV_ESCALATED: return "escalated";
    case EV_BOOT: return "boot";
  }
  return "?";
}

// ---------------- schedule queries ----------------
float dosesPerDay(uint8_t t) {
  if (t < 1 || t > MAX_TRAYS || !cfg.tray[t].alerts) return 0;
  float n = 0;
  for (auto& s : cfg.tray[t].slot)
    if (s.on) n += __builtin_popcount(s.days & 0x7F) / 7.0f;
  return n;
}

int daysLeft(uint8_t t) {
  float d = dosesPerDay(t);
  if (d <= 0) return -1;
  return (int)(max<int>(ps.stock[t], 0) / d);
}

static String daysText(uint8_t mask) {
  mask &= 0x7F;
  if (mask == 0x7F) return "daily";
  if (mask == 0x3E) return "Mon-Fri";
  if (mask == 0x41) return "weekends";
  if (!mask) return "never";
  String s;
  for (int d = 0; d < 7; d++)
    if (mask & (1 << d)) { if (s.length()) s += ","; s += DAY_SHORT[d]; }
  return s;
}

String scheduleText(uint8_t t) {
  if (!cfg.tray[t].alerts) return "No schedule (manual only)";
  String s;
  for (auto& sl : cfg.tray[t].slot) {
    if (!sl.on) continue;
    char b[8];
    snprintf(b, sizeof(b), "%02u:%02u", sl.h, sl.m);
    if (s.length()) s += "  ";
    s += String(b) + " " + daysText(sl.days);
  }
  return s.length() ? s : "No times set";
}

bool nextDose(uint8_t& tray, uint8_t& slot, uint32_t& when) {
  if (!timeValid()) return false;
  time_t now = time(nullptr);
  struct tm today;
  localtime_r(&now, &today);
  uint32_t todayKey = dayKey(today);
  uint32_t best = 0;
  for (int d = 0; d < 8; d++) {
    struct tm day = today;
    day.tm_mday += d;
    day.tm_hour = 12;
    day.tm_isdst = -1;
    mktime(&day);  // normalise
    for (int t = 1; t <= cfg.trays; t++) {
      if (!cfg.tray[t].alerts) continue;
      for (int s = 0; s < MAX_SLOTS; s++) {
        const DoseSlot& sl = cfg.tray[t].slot[s];
        if (!sl.on || !(sl.days & (1 << day.tm_wday))) continue;
        if (d == 0 && ps.slotDay[t][s] == todayKey) continue;
        uint32_t e = epochAt(day, sl.h, sl.m);
        if (e <= (uint32_t)now && d == 0) continue;
        if (!best || e < best) { best = e; tray = t; slot = s; }
      }
    }
    if (best) break;
  }
  when = best;
  return best != 0;
}

// Slots whose time already passed today are marked handled when they are new/changed,
// so editing a schedule never produces an instant "missed" or late alarm.
void markPassedSlots(const Config* old) {
  if (!timeValid()) { g_freshState = true; return; }
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  uint32_t today = dayKey(lt);
  int nowMin = lt.tm_hour * 60 + lt.tm_min;
  for (int t = 1; t <= MAX_TRAYS; t++) {
    for (int s = 0; s < MAX_SLOTS; s++) {
      const DoseSlot& sl = cfg.tray[t].slot[s];
      bool changed = !old;
      if (old) {
        const DoseSlot& o = old->tray[t].slot[s];
        changed = o.on != sl.on || o.h != sl.h || o.m != sl.m || o.days != sl.days ||
                  (!old->tray[t].alerts && cfg.tray[t].alerts) || t > old->trays;
      }
      if (changed && nowMin - (sl.h * 60 + sl.m) > 1) ps.slotDay[t][s] = today;
    }
    if (t > cfg.trays || !cfg.tray[t].alerts) {
      if (act[t].st == DS_DUE) act[t] = ActiveDose();
    }
  }
  persistDirty();
}

// ---------------- mode ----------------
static bool anyState(DoseState st, int8_t person = -1) {
  for (int t = 1; t <= MAX_TRAYS; t++)
    if (act[t].st == st && (person < 0 || cfg.tray[t].person == person)) return true;
  return false;
}

static bool ringing() {
  for (int t = 1; t <= MAX_TRAYS; t++)
    if (act[t].st == DS_DUE && !act[t].snoozeUntil) return true;
  return false;
}

static int8_t earliestDueTray(bool includeSnoozed) {
  int8_t best = -1;
  for (int t = 1; t <= MAX_TRAYS; t++) {
    if (act[t].st != DS_DUE) continue;
    if (!includeSnoozed && act[t].snoozeUntil) continue;
    if (best < 0 || act[t].due < act[best].due) best = t;
  }
  return best;
}

int8_t alertPerson() {
  int8_t t = earliestDueTray(false);
  return t < 0 ? -1 : cfg.tray[t].person;
}

uint32_t alertDueTime() {
  int8_t t = earliestDueTray(true);
  return t < 0 ? 0 : act[t].due;
}

AppMode appMode() {
  if (dispensingPerson >= 0 || anyState(DS_DISPENSING)) return M_DISPENSING;
  bool ring = ringing();
  if (readyPerson >= 0 && (!ring || millis() - readyShownAt < 60000)) return M_READY;
  if (ring) return M_ALERT;
  if (anyState(DS_DUE)) return M_SNOOZED;
  return M_IDLE;
}

// ---------------- notifications ----------------
static void notifyLowStock(uint8_t t) {
  int dl = daysLeft(t);
  if (ps.lowNotified[t]) return;
  bool low = ps.stock[t] <= 0 || (dl >= 0 && dl <= cfg.lowStockDays);
  if (!low) return;
  ps.lowNotified[t] = true;
  persistDirty();
  String m = String("📦 ") + trayLabel(t) + " for " + personName(cfg.tray[t].person) + " is running low: " +
             ps.stock[t] + " dose(s) left";
  if (dl >= 0) m += String(" (~") + dl + " day" + (dl == 1 ? "" : "s") + ")";
  m += ". Please refill.";
  notify(TG_ALL, m, NL_IMPORTANT);
  uiToast(LV_SYMBOL_WARNING " Pills running low - refill soon");
}

// ---------------- demo mode (presentations) ----------------
// A test dose a minute from now runs the real cycle: alarm, dispense, confirm, history, notifications.
// "Fast" test doses tell the family after 1 min and count as missed after 2 min.
static uint32_t demoAt = 0, demoFastDue = 0;
static bool demoFastNext = false;
uint32_t demoDueAt() { return demoAt; }

static void demoStart(int secs, bool fast) {
  if (secs <= 0) { demoAt = 0; uiToast("Test dose cancelled"); return; }
  demoAt = time(nullptr) + secs;
  demoFastNext = fast;
  uiToast(String(LV_SYMBOL_BELL " Test dose in ") + (secs >= 60 ? String(secs / 60) + " min" : String(secs) + " s"));
}

// ---------------- party mode ----------------
// Party (song + disco LEDs) or Shabad (Gurbani + calm saffron light). Anything important (a dose, SOS,
// a family message) ends it; party stops by itself after 30 min, shabad after 60 min.
static const char* musicName(uint8_t k) { return k == MUSIC_SHABAD ? "Shabad" : "Party mode"; }

static void partyStop(bool quiet = false) {
  if (!g_party) return;
  uint8_t was = g_party;
  g_party = MUSIC_OFF;
  stopAudio();
  if (!quiet) uiToast(String(LV_SYMBOL_STOP " ") + musicName(was) + " stopped");
}

static void partyStart(uint8_t kind = MUSIC_PARTY) {
  if (kind != MUSIC_SHABAD) kind = MUSIC_PARTY;
  if (g_party == kind) return;
  if (g_sos.on || appMode() != M_IDLE) { uiToast(LV_SYMBOL_WARNING " Not now: medicine or SOS comes first"); return; }
  if (g_party) stopAudio();  // switching between party and shabad
  g_party = kind;
  partyUntil = millis() + (kind == MUSIC_SHABAD ? 60UL : 30UL) * 60000UL;
  if (cfg.mute) uiToast(LV_SYMBOL_MUTE " Sound is muted - lights only");
  playParty(kind);
  uiWake();
}

// ---------------- dose lifecycle ----------------
static void startDose(uint8_t t, uint8_t s, uint32_t due) {
  partyStop(true);  // medicine first
  ActiveDose& a = act[t];
  if (a.st == DS_DUE) logEvent(EV_MISSED, t, a.slot);
  if (a.st == DS_READY) logEvent(EV_UNCONFIRMED, t, a.slot);
  a = ActiveDose();
  a.st = DS_DUE;
  a.slot = s;
  a.due = due;
  logEvent(EV_DUE, t, s);
  ringNow = true;
  if (dueBatch.length()) dueBatch += "\n";
  dueBatch += String("• ") + personName(cfg.tray[t].person) + ": " + trayLabel(t);
  if (cfg.tray[t].slot[s].note.length()) dueBatch += " - " + cfg.tray[t].slot[s].note;
  uiWake();
}

static void schedulerTick() {
  if (!timeValid()) return;
  if (g_freshState) { g_freshState = false; markPassedSlots(nullptr); }
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  uint32_t today = dayKey(lt);
  int nowMin = lt.tm_hour * 60 + lt.tm_min;

  for (int t = 1; t <= cfg.trays; t++) {
    TrayCfg& tc = cfg.tray[t];
    if (!tc.alerts) continue;
    for (int s = 0; s < MAX_SLOTS; s++) {
      DoseSlot& sl = tc.slot[s];
      if (!sl.on || !(sl.days & (1 << lt.tm_wday)) || ps.slotDay[t][s] == today) continue;
      int sm = sl.h * 60 + sl.m;
      if (nowMin < sm) continue;
      ps.slotDay[t][s] = today;
      persistDirty();
      if (nowMin - sm >= cfg.missAfter) {  // device was off / offline during the whole window
        logEvent(EV_MISSED, t, s);
        continue;
      }
      startDose(t, s, epochAt(lt, sl.h, sl.m));
    }
  }
  if (demoAt && (uint32_t)now >= demoAt) {
    demoAt = 0;
    uint8_t nt = 1, ns = 0;
    uint32_t w;
    if (!nextDose(nt, ns, w) || nt != 1)
      for (int s = MAX_SLOTS - 1; s >= 0; s--) if (cfg.tray[1].slot[s].on) ns = s;
    if (act[1].st == DS_NONE) {
      startDose(1, ns, now);
      demoFastDue = demoFastNext ? (uint32_t)now : 0;
    }
  }
  if (dueBatch.length()) {
    if (cfg.tgDue) tgSend(TG_PATIENT, String("💊 Time for your pills (") + hhmm(now) + ")\n" + dueBatch, true);
    dueBatch = "";
  }

  // daily check-in: nobody touched the dispenser, pressed the button or took a dose today
  if (cfg.checkin && ps.checkinDay != today && nowMin >= cfg.checkinH * 60 + cfg.checkinM) {
    ps.checkinDay = today;
    persistDirty();
    struct tm mid = lt;
    mid.tm_hour = mid.tm_min = mid.tm_sec = 0;
    mid.tm_isdst = -1;
    if (ps.lastInteract < (uint32_t)mktime(&mid)) {
      const String who = personName(cfg.tray[1].person);
      notify(TG_CARE, String("👀 Daily check-in: no activity from ") + who + " today (no touch, button or dose since midnight). You may want to call " + who + ".", NL_IMPORTANT);
    }
  }

  // daily summary
  if (cfg.tgDaily && ps.lastDailyDay != today && nowMin >= cfg.dailyH * 60 + cfg.dailyM) {
    ps.lastDailyDay = today;
    persistDirty();
    if (cfg.tgToken.length()) tgSend(TG_CARE, statusText());
  }
}

static void alertTick() {
  uint32_t now = time(nullptr);
  for (int t = 1; t <= MAX_TRAYS; t++) {
    ActiveDose& a = act[t];
    const String who = personName(cfg.tray[t].person);
    if (a.st == DS_DUE) {
      bool fast = demoFastDue && a.due == demoFastDue;
      uint32_t escS = fast ? 60 : cfg.escalateAfter * 60UL, missS = fast ? 120 : cfg.missAfter * 60UL;
      if (a.snoozeUntil && now >= a.snoozeUntil) { a.snoozeUntil = 0; ringNow = true; uiWake(); }
      if (!a.escalated && escS && now - a.due >= escS) {
        a.escalated = true;
        logEvent(EV_ESCALATED, t, a.slot);
        notify(TG_CARE, String("⚠️ ") + who + " has not taken the " + hhmm(a.due) + " dose yet (" + trayLabel(t) +
                            ", " + (now - a.due) / 60 + " min late).", NL_IMPORTANT, KB_DOSE);
      }
      if (now - a.due >= missS) {
        logEvent(EV_MISSED, t, a.slot);
        notify(TG_ALL, String("❌ Missed dose: ") + who + " did not take " + trayLabel(t) + " (due " + hhmm(a.due) + ").", NL_URGENT);
        a = ActiveDose();
      }
    } else if (a.st == DS_READY && now - a.dispensedAt >= cfg.confirmTimeout * 60UL) {
      logEvent(EV_UNCONFIRMED, t, a.slot);
      notify(TG_CARE, String("❓ Pills were dispensed for ") + who + " at " + hhmm(a.dispensedAt) +
                          " but nobody confirmed they were taken (" + trayLabel(t) + ").", NL_IMPORTANT);
      a = ActiveDose();
    }
  }

  // keep the "pills ready" screen pointed at someone who still has pills waiting
  if (readyPerson >= 0 && dispensingPerson < 0) {
    if (!readyIsManual && !anyState(DS_READY, readyPerson)) {
      int8_t other = -1;
      for (int t = 1; t <= MAX_TRAYS; t++)
        if (act[t].st == DS_READY) { other = cfg.tray[t].person; break; }
      if (other >= 0) { readyPerson = other; readyShownAt = millis(); }
      else if (millis() - readyShownAt > 60000) readyPerson = -1;
    } else if (readyIsManual && millis() - readyShownAt > 60000) {
      readyPerson = -1;
    }
  }

  AppMode m = appMode();
  if (m == M_ALERT) {
    if (ringNow || millis() - lastSoundMs >= cfg.remindEvery * 60000UL) {
      ringNow = false;
      lastSoundMs = millis();
      int8_t t = earliestDueTray(false);
      if (t > 0 && cfg.tray[t].voice) playVoice(cfg.tray[t].voice);
      else playTrack(TRACK_ALERT);
    }
  }
  if (g_sos.on && !g_sos.ack.length() && m != M_DISPENSING && millis() - lastSosSound > 30000) {
    lastSosSound = millis();
    playTrack(TRACK_ALERT);
  }
  if (g_party && (m != M_IDLE || (int32_t)(millis() - partyUntil) > 0)) partyStop();
  if (millis() < identifyUntil) ledPattern(4, 0);
  else if (g_sos.on) ledPattern(3, 0xFF0000);
  else if (g_party == MUSIC_SHABAD) ledPattern(6, 0xFF7A00);  // kesri
  else if (g_party) ledPattern(5, 0);
  else if (m == M_IDLE && g_msg.unread) ledPattern(2, 0x00FFFF);
  else switch (m) {
    case M_DISPENSING: ledPattern(3, PERSON_RGB[dispensingPerson >= 0 ? dispensingPerson : 0]); break;
    case M_READY: ledPattern(1, PERSON_RGB[readyPerson]); break;
    case M_ALERT: ledPattern(2, PERSON_RGB[alertPerson()]); break;
    default: ledPattern(0, 0);
  }
}

// ---------------- actions ----------------
static const char* const NO_SERVO_BOARD = "Servo board not found - check base PCB cable and power";

// Every servo command checks this: never report "dispensed" when the PCA9685 isn't there
static bool servoReady() {
  if (servoBoardOk()) return true;
  uiToast(NO_SERVO_BOARD);
  return false;
}

static void dispenseDue(int8_t person) {
  if (servoBusy() || dispensingPerson >= 0 || !servoReady()) return;
  if (person < 0) {
    int8_t t = earliestDueTray(false);
    if (t < 0) t = earliestDueTray(true);
    if (t < 0) return;
    person = cfg.tray[t].person;
  }
  bool any = false;
  for (int t = 1; t <= MAX_TRAYS; t++) {
    if (act[t].st == DS_DUE && cfg.tray[t].person == person) {
      act[t].st = DS_DISPENSING;
      act[t].snoozeUntil = 0;
      servoQueue(t, J_DOSE);
      any = true;
    }
  }
  if (any) { dispensingPerson = person; groupHasScheduled = true; uiWake(); }
}

const char* manualCheck(uint8_t t) {
  static char why[64];
  if (t < 1 || t > cfg.trays) return "Tray not installed";
  if (!servoBoardOk()) return NO_SERVO_BOARD;
  if (servoBusy()) return "Busy dispensing";
  if (act[t].st == DS_DUE) return nullptr;
  if (ps.stock[t] <= 0) return "Tray is empty - refill first";
  if (cfg.maxPerDay && timeValid()) {
    uint32_t now = time(nullptr);
    int n = 0;
    for (uint32_t d : ps.dispTimes) if (d && now - d < 86400UL) n++;
    if (n >= cfg.maxPerDay) {
      snprintf(why, sizeof(why), "Safety limit: %d doses in 24 h already (max %u)", n, cfg.maxPerDay);
      return why;
    }
  }
  uint32_t now = time(nullptr);
  if (cfg.minGap && ps.lastDisp[t] && timeValid() && now - ps.lastDisp[t] < cfg.minGap * 60UL) {
    snprintf(why, sizeof(why), "Safety lock: dispensed %lu min ago (min gap %u)",
             (unsigned long)((now - ps.lastDisp[t]) / 60), cfg.minGap);
    return why;
  }
  return nullptr;
}

static void manualDispense(uint8_t t, bool force) {
  if (!servoReady()) return;  // "force" overrides the safety lock, never a missing servo board
  const char* why = manualCheck(t);
  if (why && !(force && t >= 1 && t <= cfg.trays && !servoBusy())) { uiToast(why); return; }
  uint8_t p = cfg.tray[t].person;
  if (act[t].st == DS_DUE) {
    act[t].st = DS_DISPENSING;
    groupHasScheduled = true;
  } else {
    manualKind[t] = EV_MANUAL;
    manualSlot[t] = 0xFF;
    groupHasScheduled = false;
    if (timeValid() && cfg.tray[t].alerts && cfg.earlyWindow) {
      time_t now = time(nullptr);
      struct tm lt;
      localtime_r(&now, &lt);
      int nowMin = lt.tm_hour * 60 + lt.tm_min;
      for (int s = 0; s < MAX_SLOTS; s++) {
        const DoseSlot& sl = cfg.tray[t].slot[s];
        int sm = sl.h * 60 + sl.m;
        if (sl.on && (sl.days & (1 << lt.tm_wday)) && ps.slotDay[t][s] != dayKey(lt) && sm >= nowMin &&
            sm - nowMin <= cfg.earlyWindow) {
          ps.slotDay[t][s] = dayKey(lt);  // this counts as the upcoming dose
          manualKind[t] = EV_EARLY;
          manualSlot[t] = s;
          break;
        }
      }
    }
  }
  servoQueue(t, J_DOSE);
  dispensingPerson = p;
  uiWake();
}

// person: -1 = every dose waiting in the cup, -2 = only close the manual "pills ready" screen
static void markTaken(int8_t person, uint8_t src) {
  String trays;
  uint32_t now = time(nullptr);
  for (int t = 1; person != -2 && t <= MAX_TRAYS; t++) {
    if (act[t].st != DS_READY || (person >= 0 && cfg.tray[t].person != person)) continue;
    logEvent(EV_TAKEN, t, act[t].slot);
    if (trays.length()) trays += ", ";
    trays += trayLabel(t);
    if (person < 0) person = cfg.tray[t].person;
    act[t] = ActiveDose();
  }
  if (src == SRC_DEVICE) ps.lastInteract = now;
  if (trays.length())
    tgSend(TG_CARE, String("✅ ") + personName(person) + " took their pills at " + hhmm(now) + " - " + trays +
                        (src == SRC_TELEGRAM ? " (confirmed via Telegram)" : ""));
  readyPerson = -1;
  for (int t = 1; t <= MAX_TRAYS; t++)
    if (act[t].st == DS_READY) { readyPerson = cfg.tray[t].person; readyShownAt = millis(); break; }
}

static void snoozeAll() {
  uint32_t until = time(nullptr) + cfg.snoozeMin * 60UL;
  bool any = false;
  for (int t = 1; t <= MAX_TRAYS; t++)
    if (act[t].st == DS_DUE && !act[t].snoozeUntil) { act[t].snoozeUntil = until; logEvent(EV_SNOOZED, t, act[t].slot); any = true; }
  if (any) uiToast(String(LV_SYMBOL_BELL " Snoozed until ") + hhmm(until));
}

static void skipDue(int8_t person) {
  if (person < 0) {
    int8_t t = earliestDueTray(true);
    if (t < 0) return;
    person = cfg.tray[t].person;
  }
  String trays;
  for (int t = 1; t <= MAX_TRAYS; t++) {
    if (act[t].st != DS_DUE || cfg.tray[t].person != person) continue;
    logEvent(EV_SKIPPED, t, act[t].slot);
    if (trays.length()) trays += ", ";
    trays += trayLabel(t);
    act[t] = ActiveDose();
  }
  if (trays.length()) tgSend(TG_CARE, String("⏭ Dose skipped for ") + personName(person) + ": " + trays);
}

static void refill(uint8_t t, int n) {
  if (t < 1 || t > MAX_TRAYS) return;
  ps.stock[t] = n > 0 ? min<int>(n, cfg.capacity) : cfg.capacity;
  ps.lowNotified[t] = false;
  persistDirty();
  logEvent(EV_REFILL, t);
  uiToast(String("Refilled: ") + ps.stock[t] + " doses");
}

static void servoDone(uint8_t t, JobKind kind) {
  if (kind != J_DOSE) return;
  Lock l;
  uint32_t now = time(nullptr);
  ps.stock[t] = max<int>(0, ps.stock[t] - 1);
  ps.lastDisp[t] = timeValid() ? now : 0;
  if (timeValid()) {  // keep the 8 most recent dispense times
    memmove(&ps.dispTimes[1], &ps.dispTimes[0], sizeof(ps.dispTimes) - sizeof(ps.dispTimes[0]));
    ps.dispTimes[0] = now;
  }
  persistDirty();
  // family message, sent when the whole group has dropped (below)
  static String dispMsg;
  String what;
  if (act[t].st == DS_DISPENSING) what = String("the ") + hhmm(act[t].due) + " dose";
  else if (manualKind[t] == EV_EARLY && manualSlot[t] < MAX_SLOTS) {
    char b[8];
    snprintf(b, sizeof(b), "%02u:%02u", cfg.tray[t].slot[manualSlot[t]].h, cfg.tray[t].slot[manualSlot[t]].m);
    what = String("the ") + b + " dose, a little early";
  } else what = "an extra dose";
  if (dispMsg.length()) dispMsg += "\n";
  dispMsg += String("• ") + trayLabel(t) + " - " + what + " (" + ps.stock[t] + " doses left)";
  if (act[t].st == DS_DISPENSING) {
    logEvent(EV_DISPENSED, t, act[t].slot);
    if (cfg.requireConfirm) { act[t].st = DS_READY; act[t].dispensedAt = now; }
    else act[t] = ActiveDose();
  } else {
    logEvent(manualKind[t] ? manualKind[t] : EV_MANUAL, t, manualSlot[t]);
  }
  notifyLowStock(t);
  if (!servoBusy()) {  // whole group finished
    bool waitConfirm = groupHasScheduled && cfg.requireConfirm;
    tgSend(TG_CARE, String("💊 Pills dispensed for ") + personName(dispensingPerson >= 0 ? dispensingPerson : cfg.tray[t].person) +
                        (timeValid() ? " at " + hhmm(now) : String("")) + "\n" + dispMsg +
                        (waitConfirm ? "\n⏳ Waiting for them to tap \"I took them\"." : ""));
    dispMsg = "";
    readyPerson = dispensingPerson;
    readyIsManual = !groupHasScheduled || !cfg.requireConfirm;
    dispensingPerson = -1;
    readyShownAt = millis();
    playTrack(TRACK_READY);
    uiWake();
  }
}

// ---------------- SOS & family messages ----------------
static String patientName() { return personName(cfg.tray[1].person); }

static void sosTrigger(uint8_t src) {
  if (g_sos.on) return;
  partyStop(true);
  uint32_t now = time(nullptr);
  g_sos.on = true;
  g_sos.at = now;
  g_sos.ack = "";
  ps.lastInteract = now;
  lastSosSound = millis();
  logEvent(EV_SOS, 0);
  playTrack(TRACK_ALERT);
  notify(TG_ALL, String("🆘 SOS! ") + patientName() + " pressed the help button" + (g_timeOk ? " at " + hhmm(now) : String("")) +
                     (src == SRC_DEVICE ? "" : " (from the web portal)") + ". Please check on them now.", NL_URGENT, KB_SOS);
  uiWake();
}

static void sosClear(const String& by) {
  if (!g_sos.on) return;
  g_sos.on = false;
  notify(TG_ALL, by.length() ? String("✅ SOS cleared by ") + by + "."
                             : String("✅ ") + patientName() + " says they are OK. SOS cancelled.", NL_IMPORTANT);
  uiToast(LV_SYMBOL_OK " SOS cleared");
}

static void sosComing(const String& who) {
  if (!g_sos.on) return;
  g_sos.ack = who.length() ? who : String("Family");
  playTrack(TRACK_READY);
  notify(TG_CARE, String("🏃 ") + g_sos.ack + " is on the way to " + patientName() + ".", NL_INFO);
  uiWake();
}

// s = "chat\nfrom\ntext" (chat empty when sent from the web portal)
static void familyMessage(const char* raw) {
  String in(raw);
  int a = in.indexOf('\n'), b = a >= 0 ? in.indexOf('\n', a + 1) : -1;
  if (b < 0) return;
  String text = in.substring(b + 1);
  text.trim();
  if (!text.length()) return;
  partyStop(true);
  if (text.length() > 160) text = text.substring(0, 160);
  g_msg.unread = true;
  g_msg.at = time(nullptr);
  g_msg.readAt = 0;
  g_msg.chat = in.substring(0, a);
  g_msg.from = in.substring(a + 1, b);
  g_msg.text = text;
  playTrack(TRACK_READY);
  if (g_msg.chat.length()) tgSendTo(g_msg.chat, String("📨 Showing on the dispenser screen. You'll get a message when ") + patientName() + " reads it.");
  uiWake();
}

static void familyMessageRead() {
  if (!g_msg.unread) return;
  g_msg.unread = false;
  g_msg.readAt = time(nullptr);
  ps.lastInteract = g_msg.readAt;
  if (g_msg.chat.length())
    tgSendTo(g_msg.chat, String("👍 ") + patientName() + " read your message" + (g_timeOk ? " at " + hhmm(g_msg.readAt) : String("")) +
                             ": \"" + g_msg.text + "\"");
}

// ---------------- Telegram commands ----------------
String statusText() {
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  char hdr[48];
  strftime(hdr, sizeof(hdr), "%a %d %b, %H:%M", &lt);
  String s = String("💊 PillDispenser - ") + hdr + "\n";
  if (g_sos.on) s += String("🆘 SOS active since ") + hhmm(g_sos.at) + (g_sos.ack.length() ? " - " + g_sos.ack + " is on the way" : String("")) + "\n";
  AppMode m = appMode();
  if (m == M_ALERT || m == M_SNOOZED) {
    s += "🔔 Due now:";
    for (int t = 1; t <= MAX_TRAYS; t++)
      if (act[t].st == DS_DUE)
        s += String("\n • ") + personName(cfg.tray[t].person) + " - " + trayLabel(t) + " since " + hhmm(act[t].due) +
             (act[t].snoozeUntil ? " (snoozed)" : "");
    s += "\n";
  }
  for (int t = 1; t <= MAX_TRAYS; t++)
    if (act[t].st == DS_READY)
      s += String("⏳ In cup, not confirmed: ") + personName(cfg.tray[t].person) + " - " + trayLabel(t) + "\n";
  uint8_t nt, ns;
  uint32_t when;
  if (nextDose(nt, ns, when))
    s += String("⏭ Next: ") + personName(cfg.tray[nt].person) + " - " + trayLabel(nt) + " at " + hhmm(when) + "\n";
  s += "\n📦 Stock:";
  for (int t = 1; t <= cfg.trays; t++) {
    int dl = daysLeft(t);
    s += String("\n • ") + cfg.tray[t].name + ": " + ps.stock[t] + " left" + (dl >= 0 ? String(" (~") + dl + " days)" : "");
    if (ps.lastEvTime[t] && ps.lastEv[t]) {
      time_t e = ps.lastEvTime[t];
      struct tm el;
      localtime_r(&e, &el);
      if (dayKey(el) == dayKey(lt)) s += String(" - last: ") + eventName(ps.lastEv[t]) + " " + hhmm(e);
    }
  }
  return s;
}

static String helpText() {
  return "💊 PillDispenser by " TEAM_NAME "\n\nCommands\n"
         "/status - overview & stock\n"
         "/dispense - dispense the dose that is due\n"
         "/dispense now - extra dose right now\n"
         "/taken - confirm pills were taken\n"
         "/snooze - snooze the alarm\n"
         "/skip - skip the due dose\n"
         "/refill - tray was refilled (or /refill 20)\n"
         "/msg <text> - show a message on the screen\n"
         "/coming - tell them you are on the way (SOS)\n"
         "/ok - clear an SOS\n"
         "/party - party mode (music + disco lights)\n"
         "/shabad - play shabad Gurbani\n"
         "/stop - stop the party or shabad\n"
         "/ip - web portal address";
}

// ---------------- AI assistant (Groq) ----------------
// A family member writes a normal sentence in Telegram ("did Papa take his pills today?", "goli kab khatam hogi?").
// The dispenser sends the question with its live data and the last 7 days to the AI and replies with the answer.
static void aiAnswer(const String& chat, const String& from, String question) {
  question.trim();
  if (question.length() > 500) question = question.substring(0, 500);
  String raw, log;
  historyJson(raw, time(nullptr) - 7 * 86400);
  {
    JsonDocument h;
    if (!deserializeJson(h, raw)) {
      for (JsonArrayConst e : h.as<JsonArrayConst>()) {
        uint8_t ev = e[1];
        if (ev == EV_DUE || ev == EV_BOOT || ev == EV_SNOOZED || ev == EV_DISPENSED) continue;
        time_t t = e[0].as<uint32_t>();
        struct tm lt;
        localtime_r(&t, &lt);
        char b[24];
        strftime(b, sizeof(b), "%a %d %b %H:%M", &lt);
        log += String(b) + " " + eventName(ev) + "\n";
      }
    }
  }
  raw = "";
  if (log.length() > 2500) log = log.substring(log.indexOf('\n', log.length() - 2500) + 1);  // keep the latest
  const TrayCfg& tc = cfg.tray[1];
  String notes;
  for (int s = 0; s < MAX_SLOTS; s++)
    if (tc.slot[s].on && tc.slot[s].note.length())
      notes += String(tc.slot[s].h < 10 ? "0" : "") + tc.slot[s].h + ":" + (tc.slot[s].m < 10 ? "0" : "") + tc.slot[s].m + " - " + tc.slot[s].note + "\n";
  String sys = String("You are the assistant built into a smart pill dispenser. You answer questions from the family of ") +
               personName(tc.person) + " using only the live data below.\n"
               "Rules: reply in the same language and script as the question (Hindi, Punjabi, Urdu, English or Hinglish). "
               "Be warm and short: at most 4 sentences, plain text, no markdown or tables. If the data does not answer the "
               "question, say so. Never suggest changing doses and give no medical advice; for medical questions suggest "
               "asking their doctor. Times are local (" + cfg.tzName + ").\n\nDISPENSER NOW\n" + statusText() +
               "\nSchedule: " + tc.name + " - " + scheduleText(1) + "\n" + (notes.length() ? "Instructions:\n" + notes : String("")) +
               "Overdose lock: " + (cfg.minGap ? String(cfg.minGap) + " min between doses" : String("off")) +
               ". Missed after " + cfg.missAfter + " min.\n\nLAST 7 DAYS (time, event)\n" + (log.length() ? log : String("no events\n"));
  JsonDocument d;
  d["model"] = cfg.aiModel;
  d["temperature"] = 0.3;
  d["max_completion_tokens"] = 1024;
  JsonArray m = d["messages"].to<JsonArray>();
  JsonObject s = m.add<JsonObject>();
  s["role"] = "system";
  s["content"] = sys;
  JsonObject u = m.add<JsonObject>();
  u["role"] = "user";
  u["content"] = (from.length() ? from + " asks: " : String("")) + question;
  String body;
  serializeJson(d, body);
  if (!aiQueue(chat, body)) tgSendTo(chat, "The assistant is busy. Please try again in a minute.");
}

static void handleTelegram(const char* raw) {
  String in(raw);
  int nl = in.indexOf('\n');
  if (nl < 0) return;
  String chat = in.substring(0, nl);
  int nl2 = in.indexOf('\n', nl + 1);
  String from = nl2 > 0 ? in.substring(nl + 1, nl2) : String("");
  String text = in.substring(nl2 > 0 ? nl2 + 1 : nl + 1);
  String original = text;
  text.trim();
  text.toLowerCase();
  int at = text.indexOf('@');
  if (at > 0) { int sp = text.indexOf(' ', at); text = text.substring(0, at) + (sp > 0 ? text.substring(sp) : ""); }
  String cmd = text, arg;
  int sp = text.indexOf(' ');
  if (sp > 0) { cmd = text.substring(0, sp); arg = text.substring(sp + 1); arg.trim(); }
  int n = arg.toInt();
  if (arg == "now") n = 1;

  if (cmd == "/start" || cmd == "/help") { tgSendTo(chat, helpText() + "\n\nYour chat id: " + chat); return; }
  if (!tgAuthorized(chat)) { tgSendTo(chat, "Not authorised. Add chat id " + chat + " in the web portal."); return; }

  if (cmd == "/status" || cmd == "/stock" || cmd == "/next") tgSendTo(chat, statusText(), true);
  else if (cmd == "/dispense") {
    if (n > 0) {
      const char* why = manualCheck(n);
      if (why) tgSendTo(chat, String("⛔ ") + why);
      else { manualDispense(n, false); tgSendTo(chat, String("⚙️ Dispensing ") + trayLabel(n)); }
    } else if (anyState(DS_DUE)) {
      dispenseDue(-1);
      tgSendTo(chat, "⚙️ Dispensing the due dose. Pills will be in the cup in a few seconds.");
    } else tgSendTo(chat, "Nothing is due right now. Send /dispense now for an extra dose.");
  } else if (cmd == "/taken") {
    if (anyState(DS_READY)) { markTaken(-1, SRC_TELEGRAM); tgSendTo(chat, "✅ Recorded as taken."); }
    else tgSendTo(chat, "No dispensed pills are waiting for confirmation.");
  } else if (cmd == "/snooze") { snoozeAll(); tgSendTo(chat, String("⏰ Snoozed for ") + cfg.snoozeMin + " min."); }
  else if (cmd == "/skip") { skipDue(-1); tgSendTo(chat, "⏭ Skipped."); }
  else if (cmd == "/refill") {
    refill(1, n);  // optional number = doses now in the tray
    tgSendTo(chat, String("📦 ") + trayLabel(1) + " refilled: " + ps.stock[1] + " doses.");
  } else if (cmd == "/msg") {
    int sp2 = original.indexOf(' ');
    String msg = sp2 > 0 ? original.substring(sp2 + 1) : String("");
    msg.trim();
    if (!msg.length()) tgSendTo(chat, "Usage: /msg Please call me when you are free");
    else familyMessage((chat + "\n" + (from.length() ? from : String("Family")) + "\n" + msg).c_str());
  } else if (cmd == "/coming") {
    if (g_sos.on) { sosComing(from); tgSendTo(chat, "👍 They can see on the screen that you are coming."); }
    else tgSendTo(chat, "No SOS is active.");
  } else if (cmd == "/ok") {
    if (g_sos.on) sosClear(from.length() ? from : String("a caregiver"));
    else tgSendTo(chat, "No SOS is active.");
  } else if (cmd == "/party" || cmd == "/shabad") {
    uint8_t k = cmd == "/shabad" ? MUSIC_SHABAD : MUSIC_PARTY;
    partyStart(k);
    tgSendTo(chat, g_party == k ? (k == MUSIC_SHABAD ? "🙏 Shabad is playing on the dispenser. Send /stop to end it."
                                                      : "🪩 Party mode on! Send /stop to end it.")
                                : "Not now: a dose or SOS is active.");
  } else if (cmd == "/stop") {
    if (g_party) { tgSendTo(chat, String(musicName(g_party)) + " stopped."); partyStop(); }
    else tgSendTo(chat, "Nothing is playing.");
  } else if (cmd == "/ip") tgSendTo(chat, String("🌐 http://") + localIp() + "  or  http://" + cfg.hostname + ".local");
  else if (!cmd.startsWith("/") && cfg.aiKey.length()) aiAnswer(chat, from, original);
  else tgSendTo(chat, helpText() + (cfg.aiKey.length() ? String("") : String("\n\nTip: add a free Groq key in the web app (Settings - AI assistant) and you can ask questions in your own words.")));
}

// ---------------- command queue ----------------
bool postCmd(uint8_t type, uint8_t src, int32_t a, int32_t b, const char* s) {
  Cmd c{type, src, a, b, s ? strdup(s) : nullptr};
  if (xQueueSend(cmdQ, &c, 0) != pdTRUE) { free(c.s); return false; }
  return true;
}

static void applyConfigJson(const char* json) {
  Config* old = new Config(cfg);
  String err;
  if (!configFromJson(json, err)) { uiToast("Config error: " + err); delete old; return; }
  markPassedSlots(old);
  saveConfig();
  if (old->tz != cfg.tz) applyTimezone();
  applyDisplaySettings();
  if (old->ssid != cfg.ssid || old->pass != cfg.pass) connectWifi(cfg.ssid, cfg.pass);
  if (old->capacity != cfg.capacity)
    for (int t = 1; t <= MAX_TRAYS; t++) ps.stock[t] = min<int>(ps.stock[t], cfg.capacity);
  delete old;
  uiToast(LV_SYMBOL_OK " Settings saved");
}

static void processCmd(Cmd& c) {
  switch (c.type) {
    case C_DISPENSE_DUE: dispenseDue(c.a); break;
    case C_TAKEN: markTaken(c.a, c.src); break;
    case C_SNOOZE: snoozeAll(); break;
    case C_SKIP: skipDue(c.a); break;
    case C_MANUAL: manualDispense(c.a, c.b); break;
    case C_ADVANCE: if (servoReady() && c.a >= 1 && c.a <= cfg.trays && !servoBusy()) servoQueue(c.a, J_ADVANCE, constrain(c.b, 1, 30)); break;
    case C_ALIGN: if (servoReady() && c.a >= 1 && c.a <= cfg.trays && !servoBusy()) servoQueue(c.a, J_ALIGN); break;
    case C_REFILL: refill(c.a, c.b); break;
    case C_SOUND:
      if (c.b > 0) playVoice(c.b); else playTrack(c.a ? c.a : TRACK_READY);
      break;
    case C_TG_TEST: tgSend(TG_ALL, "👋 Test message from your PillDispenser. Telegram is working!\n\n" + helpText()); break;
    case C_TG_CMD: if (c.s) handleTelegram(c.s); break;
    case C_REBOOT: rebootAt = millis() + 1000; break;
    case C_FACTORY: factoryReset(); rebootAt = millis() + 1000; break;
    case C_CONFIG:
    case C_RESTORE: if (c.s) applyConfigJson(c.s); break;
    case C_SETTIME:
      if (c.a > 1700000000 && (!timeValid() || c.b)) {
        struct timeval tv{(time_t)c.a, 0};
        settimeofday(&tv, nullptr);
        uiToast(LV_SYMBOL_OK " Clock set");
      }
      break;
    case C_WIFI:
      if (c.s) {
        String s(c.s);
        int nl = s.indexOf('\n');
        cfg.ssid = nl >= 0 ? s.substring(0, nl) : s;
        cfg.pass = nl >= 0 ? s.substring(nl + 1) : "";
        saveConfig();
        connectWifi(cfg.ssid, cfg.pass);
      }
      break;
    case C_AP: startAP(); break;
    case C_IDENTIFY: identifyUntil = millis() + 5000; playTrack(TRACK_READY); uiWake(); break;
    case C_SOS: sosTrigger(c.src); break;
    case C_SOS_CANCEL: sosClear(c.s ? String(c.s) : String("")); break;
    case C_MSG: if (c.s) familyMessage(c.s); break;
    case C_MSG_ACK: familyMessageRead(); break;
    case C_NTEST: notifyTest(c.a); break;
    case C_DEMO: demoStart(c.a, c.b); break;
    case C_PARTY: if (c.a) partyStart(c.a); else partyStop(); break;
  }
  free(c.s);
}

static void buttonPressed(uint8_t kind) {
  Lock l;
  AppMode m = appMode();
  ps.lastInteract = time(nullptr);
  if (kind == 2) { sosTrigger(SRC_DEVICE); return; }
  if (g_party) { partyStop(); return; }  // any press ends the party
  if (kind == 1) { if (m == M_ALERT) snoozeAll(); else uiWake(); return; }
  if (g_sos.on && m == M_IDLE) { uiWake(); return; }
  if (g_msg.unread && m == M_IDLE) { familyMessageRead(); uiWake(); return; }
  if (m == M_ALERT || m == M_SNOOZED) dispenseDue(-1);
  else if (m == M_READY) markTaken(readyIsManual ? -2 : readyPerson, SRC_DEVICE);
  else uiWake();
}

// ---------------- init / tick ----------------
void appInit() {
  cmdQ = xQueueCreate(24, sizeof(Cmd));
  onServoJobDone = servoDone;
  onButton = buttonPressed;
  for (int t = 1; t <= MAX_TRAYS; t++)  // pills restored as "in the cup" after a restart
    if (act[t].st == DS_READY) { readyPerson = cfg.tray[t].person; readyShownAt = millis(); break; }
}

void appTick() {
  Cmd c;
  while (xQueueReceive(cmdQ, &c, 0) == pdTRUE) { Lock l; processCmd(c); }
  static uint32_t lastSec = 0;
  if (millis() - lastSec >= 1000) {
    lastSec = millis();
    g_timeOk = timeValid();
    Lock l;
    schedulerTick();
    alertTick();
    // live doses survive a reboot / power cut: save whenever they change
    static uint32_t lastSig = 0;
    uint32_t sig = 0;
    for (int t = 1; t <= MAX_TRAYS; t++) sig = sig * 31 + act[t].st * 7 + act[t].due + act[t].snoozeUntil + act[t].slot;
    if (sig != lastSig) { lastSig = sig; persistDirty(); }
  }
  if (rebootAt && millis() > rebootAt) {
    savePersist();
    ESP.restart();
  }
}
