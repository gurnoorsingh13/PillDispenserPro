// PillDispenser Pro - shared declarations
// Board: "ESP32 Dev Module", Partition: "Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)", core esp32 2.0.17
#pragma once
#include <Arduino.h>
#include <time.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <lvgl.h>  // for LV_SYMBOL_* in toast messages

#define FW_NAME    "PillDispenser Pro"
#define FW_VERSION "2.4.0"
// Project credits (boot screen, device info, web app, Telegram, serial log)
#define TEAM_NAME    "Maverick Innovators"
#define TEAM_MEMBERS "Gurnoor Singh, Ansh Sharma, Samarpit Gujral, Harman Singh Gill"

// ---------- Hardware (CYD ESP32-2432S028 + original base PCB, unchanged) ----------
#define PIN_TOUCH_IRQ   36
#define PIN_TOUCH_MOSI  32
#define PIN_TOUCH_MISO  39
#define PIN_TOUCH_CLK   25
#define PIN_TOUCH_CS    33
#define PIN_I2C_SDA     22
#define PIN_I2C_SCL     27
#define PIN_LED         16
#define PIN_BUTTON      4
#define PIN_BACKLIGHT   21
#define PIN_DFPLAYER_TX 17
#define NUM_LEDS        2
#define SERVO_PULSE_MIN 150   // PCA9685 counts (of 4096) for 0 deg
#define SERVO_PULSE_MAX 600   // PCA9685 counts for 180 deg
#define SERVO_FREQ      60

// Single-tray build: the servo is plugged straight into the base PCB (no tray stack).
// The servo signal is sent to all SERVO_HEADERS tray headers, so it can be plugged into any of them
// (the base PCB's "Tray 1" header = PCA9685 output 0 ... "Tray 10" = output 9).
#define MAX_TRAYS  1
#define SERVO_HEADERS 10
// 1 = build without Wi-Fi (touch screen only). Build with esp32 core 2.0.17: Wi-Fi crashes on 3.x
#define NO_WIFI 0
#define MAX_SLOTS  4
#define MAX_PEOPLE 8

#define TRACK_ALERT 1   // SD root: 1-Alert.mp3
#define TRACK_READY 2   // SD root: 2-PillsReady.mp3
// Custom voice prompts: SD folder /01/001.mp3 ... /01/099.mp3 (tray "voice" = file number)

// ---------- Configuration (saved to /config.json) ----------
struct DoseSlot {
  bool on = false;
  uint8_t h = 8, m = 0;
  uint8_t days = 0x7F;  // bit0 = Sunday ... bit6 = Saturday
  String note;          // instructions, e.g. "2 tablets - after food"
};

struct TrayCfg {
  String name;
  uint8_t person = 0;   // index into people[] (also the colour)
  bool alerts = false;  // scheduled alarms enabled
  uint8_t voice = 0;    // 0 = default alert sound, N = /01/00N.mp3
  DoseSlot slot[MAX_SLOTS];
};

struct Config {
  // device
  String hostname = "pilldisp";
  uint8_t trays = 1;                 // installed trays (stacked from 1)
  String tz = "IST-5:30";            // POSIX TZ string (India by default; the web app offers the phone's zone)
  String tzName = "India";
  uint8_t volume = 20;               // 1..30
  bool mute = false;
  uint8_t brightness = 80;           // 5..100 %
  bool invert = false;               // flip panel colour inversion (for "INV" screens)
  bool clockSaver = true;            // show clock when idle, otherwise backlight off
  uint16_t screenTimeout = 120;      // seconds
  bool quiet = false;                // quiet hours
  uint8_t quietFrom = 22, quietTo = 7;
  uint8_t quietVolume = 8;
  // alerts
  uint8_t remindEvery = 1;           // minutes between alarm sounds
  uint8_t snoozeMin = 10;
  uint16_t escalateAfter = 15;       // minutes late before caregivers are notified (0 = off)
  uint16_t missAfter = 120;          // minutes before a dose is marked missed
  bool requireConfirm = true;        // ask "did you take them?" after dispensing
  uint16_t confirmTimeout = 30;      // minutes
  uint16_t minGap = 120;             // minimum minutes between two dispenses of the same tray (0 = off)
  uint16_t earlyWindow = 90;         // a manual dispense this close to a scheduled dose counts as that dose
  uint8_t maxPerDay = 0;             // as-needed limit: max dispenses in any 24 h (0 = off)
  bool checkin = false;              // tell caregivers when there is no activity by checkinH:checkinM
  uint8_t checkinH = 11, checkinM = 0;
  // stock
  uint8_t capacity = 30;
  uint8_t lowStockDays = 3;
  // security
  String pin;                        // 4-6 digits, empty = none
  bool pinDispense = false;
  bool pinSettings = true;
  String webPass;                    // empty = open web portal (user "admin")
  // telegram
  String tgToken, tgPatient, tgCare; // chat ids, comma separated
  bool tgDue = true, tgDaily = true;
  uint8_t dailyH = 21, dailyM = 0;
  // more channels (all optional)
  String ntfyTopic;                  // ntfy.sh push notifications to the ntfy phone app
  String waPhone, waKey;             // WhatsApp via CallMeBot: caregiver alerts
  String callUser;                   // Telegram @username for CallMeBot voice calls (SOS, missed doses)
  String hookUrl;                    // webhook: every notification POSTed as JSON
  // AI assistant (Groq, OpenAI-compatible API): Telegram questions on the device, scan / ask / summary in the web app
  String aiKey;
  String aiModel = "openai/gpt-oss-120b";
  String aiVision = "qwen/qwen3.8-27b";
  // mechanics
  uint8_t servoRest = 70, servoPush = 180;
  uint16_t pushMs = 300, restMs = 500;
  uint8_t cyclesPerDose = 2;
  // wifi
  String ssid, pass;
  String ssid2, pass2;               // backup network, e.g. a phone hotspot
  // people & trays (index 1..10 used)
  String people[MAX_PEOPLE];
  TrayCfg tray[MAX_TRAYS + 1];
};

// ---------- Persistent runtime state (saved to /state.json) ----------
struct Persist {
  int16_t stock[MAX_TRAYS + 1];
  uint32_t lastDisp[MAX_TRAYS + 1];
  uint32_t slotDay[MAX_TRAYS + 1][MAX_SLOTS];  // yyyymmdd the slot was last handled
  bool lowNotified[MAX_TRAYS + 1];
  uint8_t lastEv[MAX_TRAYS + 1];
  uint32_t lastEvTime[MAX_TRAYS + 1];
  uint8_t slotEv[MAX_TRAYS + 1][MAX_SLOTS];      // outcome of each dose slot (EV_TAKEN, EV_MISSED, ...)
  uint32_t slotEvDay[MAX_TRAYS + 1][MAX_SLOTS];  // yyyymmdd of that outcome
  uint32_t lastDailyDay;
  uint32_t dispTimes[8];             // recent dispense times (as-needed 24 h limit)
  uint32_t lastInteract;             // last touch / button press / confirmation (daily check-in)
  uint32_t checkinDay;
};

// ---------- Events / history ----------
enum EventType : uint8_t {
  EV_DUE = 1, EV_DISPENSED, EV_TAKEN, EV_SKIPPED, EV_MISSED, EV_SNOOZED,
  EV_MANUAL, EV_EARLY, EV_REFILL, EV_UNCONFIRMED, EV_ESCALATED, EV_BOOT, EV_SOS
};
struct LogRec { uint32_t t; uint8_t ev, tray, slot, person; };

// ---------- Live dose tracking ----------
enum DoseState : uint8_t { DS_NONE = 0, DS_DUE, DS_DISPENSING, DS_READY };
struct ActiveDose {
  DoseState st = DS_NONE;
  uint8_t slot = 0;
  uint32_t due = 0, snoozeUntil = 0, dispensedAt = 0;
  bool escalated = false;
};

enum AppMode : uint8_t { M_IDLE, M_ALERT, M_SNOOZED, M_DISPENSING, M_READY };

// ---------- Commands (web / telegram / button -> main loop) ----------
enum CmdType : uint8_t {
  C_DISPENSE_DUE, C_TAKEN, C_SNOOZE, C_SKIP, C_MANUAL, C_ADVANCE, C_ALIGN, C_REFILL,
  C_SOUND, C_TG_TEST, C_TG_CMD, C_REBOOT, C_FACTORY, C_CONFIG, C_SETTIME, C_WIFI, C_AP,
  C_IDENTIFY, C_RESTORE, C_SOS, C_SOS_CANCEL, C_MSG, C_MSG_ACK, C_NTEST, C_DEMO, C_PARTY
};
enum CmdSrc : uint8_t { SRC_DEVICE = 0, SRC_WEB, SRC_TELEGRAM, SRC_API };
struct Cmd { uint8_t type, src; int32_t a, b; char* s; };

// ---------- Globals ----------
extern Config cfg;
extern Persist ps;
extern ActiveDose act[MAX_TRAYS + 1];
extern SemaphoreHandle_t g_lock;   // guards cfg / ps / act between tasks
extern int8_t readyPerson;         // person whose pills are in the cup (-1 none)
extern bool readyIsManual;
extern volatile bool g_timeOk;
extern bool g_freshState;          // no saved state yet: mark today's past slots once the clock is set
extern const uint32_t PERSON_RGB[MAX_PEOPLE];
extern const char* const COLOR_NAMES[MAX_PEOPLE];
extern bool g_safeMode;            // repeated crashes: network stays off so the dispenser keeps working
#define NET_OFF (NO_WIFI || g_safeMode)
struct SosState { bool on = false; uint32_t at = 0; String ack; };
extern SosState g_sos;             // help button pressed
struct FamilyMsg { bool unread = false; uint32_t at = 0, readAt = 0; String text, from, chat; };
extern FamilyMsg g_msg;            // message from family shown on the screen
extern String g_lastTest;          // result of the last notification test (web)
// Music modes (any dose, SOS or family message stops them): 0 off, 1 party (song + disco LEDs), 2 shabad (Gurbani + calm saffron light)
enum MusicKind : uint8_t { MUSIC_OFF = 0, MUSIC_PARTY = 1, MUSIC_SHABAD = 2 };
extern uint8_t g_party;

struct Lock {
  Lock() { xSemaphoreTakeRecursive(g_lock, portMAX_DELAY); }
  ~Lock() { xSemaphoreGiveRecursive(g_lock); }
};

// app.cpp - dose engine
void appInit();
void appTick();
bool postCmd(uint8_t type, uint8_t src = SRC_DEVICE, int32_t a = 0, int32_t b = 0, const char* s = nullptr);
AppMode appMode();
int8_t alertPerson();                  // person currently being alerted (-1 none)
uint32_t alertDueTime();
bool nextDose(uint8_t& tray, uint8_t& slot, uint32_t& when);
float dosesPerDay(uint8_t tray);
int daysLeft(uint8_t tray);            // -1 = no schedule
const char* manualCheck(uint8_t tray); // nullptr if allowed, else reason
String scheduleText(uint8_t tray);
String statusText();                   // human readable summary (telegram)
void markPassedSlots(const Config* old);
uint32_t dayKey(const struct tm& t);
bool timeValid();
const char* eventName(uint8_t ev);
uint32_t demoDueAt();                  // demo dose scheduled for this time (0 = none)

// storage.cpp
void storageInit();
void loadConfig();
void saveConfig();
void loadPersist();
void savePersist();
void persistDirty();
void storageTick();
void configToJson(String& out, bool secrets);
bool configFromJson(const char* json, String& err);
void logEvent(uint8_t ev, uint8_t tray, uint8_t slot = 0xFF);
void historyJson(String& out, uint32_t since);
void factoryReset();

// hardware.cpp - servos, LEDs, sound, button, backlight
enum JobKind : uint8_t { J_HOME, J_DOSE, J_ADVANCE, J_ALIGN };
void hwInit();
void hwTick();
bool servoQueue(uint8_t tray, JobKind kind, uint8_t steps = 1);
bool servoBusy();
bool servoBoardOk();  // PCA9685 answered on I2C (re-checked every few seconds while missing)
void playTrack(uint8_t track);
void playVoice(uint8_t file);
void audioReset();
void playParty(uint8_t kind);  // every file in SD folder /02/ (party) or /03/ (shabad) in turn, on repeat
void stopAudio();
void ledSet(uint32_t rgb);
void ledPattern(uint8_t pattern, uint32_t rgb);  // 0 off, 1 solid, 2 slow flash, 3 fast flash, 4 rainbow, 5 disco, 6 slow breathing
void backlight(uint8_t pct);
uint8_t effectiveVolume();
void applyDisplaySettings();
extern void (*onServoJobDone)(uint8_t tray, JobKind kind);
extern void (*onButton)(uint8_t kind);  // 0 short press, 1 held 1.5 s, 2 held 5 s (SOS)

// net.cpp - wifi, AP, time, mDNS
void netInit();
void netTick();
bool netConnected();
bool apActive();
String apSsid();
String apPass();
String localIp();
void startAP();
void connectWifi(const String& ssid, const String& pass);
void applyTimezone();
void scanStart();
int scanResults(String& json);  // -1 running, else count
void netDiag();

// web.cpp
void webInit();

// telegram.cpp - notification hub: Telegram, ntfy push, WhatsApp, voice call, webhook (one task, one TLS at a time)
enum TgTarget : uint8_t { TG_PATIENT = 1, TG_CARE = 2, TG_ALL = 3 };
enum NotifyLevel : uint8_t { NL_INFO = 0, NL_IMPORTANT, NL_URGENT };  // urgent also places a voice call
enum KbKind : uint8_t { KB_NONE = 0, KB_DOSE, KB_SOS };
enum Channel : uint8_t { CH_ALL = 0, CH_TELEGRAM, CH_NTFY, CH_WHATSAPP, CH_CALL, CH_WEBHOOK };
void tgInit();
void notify(uint8_t target, const String& text, uint8_t level = NL_INFO, uint8_t kb = KB_NONE);
void notifyTest(uint8_t channel);
void tgSend(uint8_t target, const String& text, bool keyboard = false);
void tgSendTo(const String& chat, const String& text, bool keyboard = false);
bool tgAuthorized(const String& chat);
bool aiQueue(const String& chat, const String& requestJson);  // ask the AI, reply to that Telegram chat

// ui.cpp - LVGL touch screen
void uiInit();
void uiTick();
void uiBoot(int pct, const char* msg);
void uiStart();
void uiWake();
void uiToast(const String& msg);
bool uiAwake();
