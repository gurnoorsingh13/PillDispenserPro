// Touch screen UI (LVGL 8.3, 320x240 landscape)
//
// Design: calm dark charcoal for everyday screens, one strong full-screen colour per state that needs
// attention (amber = pills due, green = pills ready, red = SOS). The patient's LED colour is only a small dot.
#include "app.h"
#include <WiFi.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>

// ---------------- display / touch drivers ----------------
static TFT_eSPI tft(320, 240);  // same construction as the original firmware
static SPIClass touchSpi(VSPI);
static XPT2046_Touchscreen ts(PIN_TOUCH_CS);  // polled: GPIO36 IRQ glitches when Wi-Fi starts
static lv_disp_draw_buf_t drawBuf;
static lv_color_t buf[320 * 24];
static uint16_t tMinX = 300, tMaxX = 3800, tMinY = 220, tMaxY = 3800;
static volatile uint32_t lastActivity = 0;
static volatile bool wakeReq = false;
static bool awake = true;
static QueueHandle_t toastQ;
static bool shotActive = false;  // screenshot in progress: copy every flushed strip to USB serial

// one flushed area: 'A', x1 y1 x2 y2 (u16 LE), then run-length records [count u8][RGB565 u16 LE]
static void shotArea(const lv_area_t* a, const lv_color_t* c) {
  uint16_t hdr[4] = {(uint16_t)a->x1, (uint16_t)a->y1, (uint16_t)a->x2, (uint16_t)a->y2};
  Serial.write('A');
  Serial.write((const uint8_t*)hdr, sizeof(hdr));
  uint32_t total = (uint32_t)(a->x2 - a->x1 + 1) * (a->y2 - a->y1 + 1);
  for (uint32_t i = 0; i < total;) {
    uint16_t p = c[i].full;
    uint32_t n = 1;
    while (i + n < total && c[i + n].full == p && n < 255) n++;
    uint8_t rec[3] = {(uint8_t)n, (uint8_t)(p & 0xFF), (uint8_t)(p >> 8)};
    Serial.write(rec, 3);
    i += n;
  }
}

static void flushCb(lv_disp_drv_t* d, const lv_area_t* a, lv_color_t* c) {
  uint32_t w = a->x2 - a->x1 + 1, h = a->y2 - a->y1 + 1;
  if (shotActive) shotArea(a, c);
  tft.startWrite();
  tft.setAddrWindow(a->x1, a->y1, w, h);
  tft.pushColors((uint16_t*)&c->full, w * h, true);
  tft.endWrite();
  lv_disp_flush_ready(d);
}

static void touchCb(lv_indev_drv_t*, lv_indev_data_t* data) {
  if (ts.touched()) {
    TS_Point p = ts.getPoint();
    if (p.x < tMinX) tMinX = p.x;
    if (p.x > tMaxX) tMaxX = p.x;
    if (p.y < tMinY) tMinY = p.y;
    if (p.y > tMaxY) tMaxY = p.y;
    data->point.x = map(p.x, tMinX, tMaxX, 1, 320);
    data->point.y = map(p.y, tMinY, tMaxY, 1, 240);
    data->state = LV_INDEV_STATE_PR;
    lastActivity = millis();
    ps.lastInteract = time(nullptr);
  } else {
    data->state = LV_INDEV_STATE_REL;
  }
}

void applyDisplaySettings() {
  tft.invertDisplay(!cfg.invert);  // panel is built with TFT_INVERSION_ON; "invert" flips it for INV screens
  if (awake) backlight(cfg.brightness);
}

// ---------------- palette ----------------
#define C_BG     lv_color_hex(0x0E1110)  // charcoal with a hint of green
#define C_CARD   lv_color_hex(0x1A1F1C)
#define C_CARD2  lv_color_hex(0x272D29)
#define C_LINE   lv_color_hex(0x333B36)
#define C_TEXT   lv_color_hex(0xF1F3EF)
#define C_MUTED  lv_color_hex(0x8E978F)
#define C_BRAND  lv_color_hex(0x3CC488)  // green accent on dark (text, bars, icons)
#define C_PRI    lv_color_hex(0x1F8F5C)  // green button with white text
#define C_AMBER  lv_color_hex(0xF2A93B)
#define C_RED    lv_color_hex(0xE5483D)
#define C_INK    lv_color_hex(0x141916)  // dark text on light backgrounds
#define C_PAPER  lv_color_hex(0xF5F5F1)
#define C_READY  lv_color_hex(0x1D8A58)  // "pills ready" screen
#define C_SOSBG  lv_color_hex(0xC9342A)  // SOS screen
#define C_OK     C_BRAND
#define C_WARN   C_AMBER
#define C_BAD    C_RED
#define C_WHITE  lv_color_white()

// display versions of the LED colours (pure LED RGB looks harsh on a screen)
static const uint32_t UI_PERSON[MAX_PEOPLE] = {0xE5484D, 0xD6409F, 0x8E4EC6, 0x3E63DD, 0x12A5B8, 0x30A46C, 0xD9A400, 0xF07A1A};
static lv_color_t personColor(int p) { return lv_color_hex(UI_PERSON[p < 0 || p >= MAX_PEOPLE ? 0 : p]); }
static lv_color_t soft(lv_color_t c, uint8_t amount) { return lv_color_mix(c, C_BG, amount); }  // c blended into the background

// ---------------- widget helpers ----------------
static lv_obj_t* mkScreen(lv_color_t bg = C_BG) {
  lv_obj_t* s = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(s, bg, 0);
  lv_obj_set_style_text_color(s, C_TEXT, 0);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  return s;
}

static lv_obj_t* mkLabel(lv_obj_t* p, const lv_font_t* f, lv_color_t c, const char* t) {
  lv_obj_t* l = lv_label_create(p);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, c, 0);
  lv_label_set_text(l, t);
  return l;
}

static lv_obj_t* mkPanel(lv_obj_t* p, int x, int y, int w, int h, lv_color_t bg, int radius = 12) {
  lv_obj_t* o = lv_obj_create(p);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, bg, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_set_style_shadow_width(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

static lv_obj_t* mkCircle(lv_obj_t* p, int d, lv_color_t bg) {
  lv_obj_t* o = mkPanel(p, 0, 0, d, d, bg, LV_RADIUS_CIRCLE);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}

// button whose label inherits the button's text colour
static lv_obj_t* mkBtn(lv_obj_t* p, int x, int y, int w, int h, const char* txt, lv_color_t bg, lv_color_t fg,
                       const lv_font_t* f, lv_event_cb_t cb, int radius = 12) {
  lv_obj_t* b = lv_btn_create(p);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_bg_color(b, bg, 0);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(b, radius, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_text_color(b, fg, 0);
  lv_obj_set_style_text_font(b, f, 0);
  lv_obj_t* l = lv_label_create(b);
  lv_label_set_text(l, txt);
  lv_obj_center(l);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
  return b;
}

static void setText(lv_obj_t* l, const String& s) {
  if (strcmp(lv_label_get_text(l), s.c_str()) != 0) lv_label_set_text(l, s.c_str());
}

static void show(lv_obj_t* o, bool on) {
  if (on) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static String hhmm(uint32_t epoch) {
  time_t e = epoch;
  struct tm lt;
  localtime_r(&e, &lt);
  char b[8];
  strftime(b, sizeof(b), "%H:%M", &lt);
  return b;
}

static String relTime(int32_t secs) {
  if (secs < 60) return "now";
  int m = secs / 60;
  if (m < 60) return String(m) + " min";
  return String(m / 60) + "h " + String(m % 60) + "m";
}

// gentle "breathing" for the one button that matters on a screen
static void pulseCb(void* o, int32_t v) {
  lv_obj_set_style_transform_width((lv_obj_t*)o, v, 0);
  lv_obj_set_style_transform_height((lv_obj_t*)o, v / 2, 0);
}
static void pulse(lv_obj_t* o, int amount, uint32_t ms) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, o);
  lv_anim_set_exec_cb(&a, pulseCb);
  lv_anim_set_values(&a, 0, amount);
  lv_anim_set_time(&a, ms);
  lv_anim_set_playback_time(&a, ms);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&a);
}

// ---------------- screens ----------------
static lv_obj_t *scrSos, *scrMsg, *scrRefill, *scrParty;
static lv_obj_t *scrBoot, *scrHome, *scrAlert, *scrBusy, *scrReady, *scrTray, *scrSettings, *scrWifi,
    *scrInfo, *scrPin, *scrSaver;
static lv_obj_t* current = nullptr;

static void go(lv_obj_t* s);
static void refreshAll();

// screen preview over USB serial (for screenshots): sample data, no forced navigation
static uint32_t previewUntil = 0;
static bool previewing() { return previewUntil && (int32_t)(previewUntil - millis()) > 0; }

// boot
static lv_obj_t *bootBar, *bootMsg;

// home
static lv_obj_t *hTime, *hDate, *hWifi, *hCard, *hCardTitle, *hCardBig, *hCardText, *hPill, *hPillLbl, *hDot;
static lv_obj_t *trCard, *trDays, *trBar, *trStock, *hTodayNone, *hSosLbl;
static lv_obj_t *hChip[MAX_SLOTS], *hChipLbl[MAX_SLOTS];

// alert / busy / ready
static lv_obj_t *aWho, *aDot, *aDue, *aTrays, *aTime, *aSnooze;
static lv_obj_t *bWho;
static lv_obj_t *rWho, *rHint, *rBtnLbl;

// tray
static uint8_t curTray = 1;
static lv_obj_t *tTitle, *tPerson, *tPersonDot, *tSched, *tBar, *tStock, *tLast;

// settings
static lv_obj_t *sVol, *sBright, *sMute, *sSaver, *sInvert;

// wifi
static lv_obj_t *wDrop, *wPass, *wKb, *wStatus;
static bool wScanning = false;

// info
static lv_obj_t *iQr, *iText, *iTitle;
static bool iShowJoin = true;

// pin
static lv_obj_t *pTa, *pMsg;
static void (*pinNext)() = nullptr;
static lv_obj_t* pinBack = nullptr;

// saver
static lv_obj_t *svTime, *svDate, *svNext, *svWarn;

// toast
static lv_obj_t *toast, *toastLbl;
static uint32_t toastUntil = 0;

// ---------------- PIN gate & confirm dialog ----------------
static void withPin(bool need, void (*fn)()) {
  String pin;
  { Lock l; pin = cfg.pin; }
  if (!need || !pin.length()) { fn(); return; }
  pinNext = fn;
  pinBack = current;
  lv_textarea_set_text(pTa, "");
  lv_label_set_text(pMsg, "Enter PIN");
  go(scrPin);
}

static void (*mbYes)() = nullptr;
static const char* mbBtns[3] = {"Cancel", "OK", ""};
static void mbCb(lv_event_t* e) {
  lv_obj_t* mb = lv_event_get_current_target(e);
  uint16_t id = lv_msgbox_get_active_btn(mb);
  lv_msgbox_close_async(mb);
  if (id == 1 && mbYes) mbYes();
}
static void confirmBox(const char* title, const String& text, const char* yes, void (*fn)()) {
  mbYes = fn;
  mbBtns[1] = yes;
  lv_obj_t* mb = lv_msgbox_create(NULL, title, text.c_str(), mbBtns, false);
  lv_obj_set_width(mb, 284);
  lv_obj_set_style_bg_color(mb, C_CARD, 0);
  lv_obj_set_style_border_color(mb, C_LINE, 0);
  lv_obj_set_style_border_width(mb, 1, 0);
  lv_obj_set_style_radius(mb, 16, 0);
  lv_obj_set_style_pad_all(mb, 14, 0);
  lv_obj_set_style_text_color(mb, C_TEXT, 0);
  lv_obj_set_style_text_font(lv_msgbox_get_title(mb), &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(lv_msgbox_get_text(mb), C_MUTED, 0);
  lv_obj_t* btns = lv_msgbox_get_btns(mb);
  lv_obj_set_height(btns, 46);
  lv_obj_set_style_text_font(btns, &lv_font_montserrat_16, 0);
  lv_obj_set_style_bg_color(btns, C_CARD2, LV_PART_ITEMS);
  lv_obj_set_style_text_color(btns, C_TEXT, LV_PART_ITEMS);
  lv_obj_set_style_radius(btns, 12, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(btns, 0, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(btns, C_PRI, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_obj_set_style_text_color(btns, C_WHITE, LV_PART_ITEMS | LV_STATE_CHECKED);
  lv_btnmatrix_set_btn_ctrl(btns, 1, LV_BTNMATRIX_CTRL_CHECKED);
  lv_obj_center(mb);
  lv_obj_add_event_cb(mb, mbCb, LV_EVENT_VALUE_CHANGED, NULL);
}

// ---------------- actions ----------------
static void doDispenseDue() { postCmd(C_DISPENSE_DUE, SRC_DEVICE, -1); }
static void doSkip() { postCmd(C_SKIP, SRC_DEVICE, -1); go(scrHome); }
static void doManual() { postCmd(C_MANUAL, SRC_DEVICE, curTray, 0); }
static void doManualForce() { postCmd(C_MANUAL, SRC_DEVICE, curTray, 1); }
static void openSettings() { go(scrSettings); }

static bool pinForDispense() { Lock l; return cfg.pinDispense; }
static bool pinForSettings() { Lock l; return cfg.pinSettings; }

// ---------------- BOOT ----------------
// logo: a capsule, half green, half white
static lv_obj_t* mkLogo(lv_obj_t* p, int w, int h) {
  lv_obj_t* cap = mkPanel(p, 0, 0, w, h, C_WHITE, LV_RADIUS_CIRCLE);
  lv_obj_set_style_clip_corner(cap, true, 0);
  mkPanel(cap, 0, 0, w / 2, h, C_PRI, 0);
  return cap;
}

static void buildBoot() {
  scrBoot = mkScreen();
  lv_obj_t* logo = mkLogo(scrBoot, 60, 26);
  lv_obj_align(logo, LV_ALIGN_CENTER, 0, -58);
  lv_obj_t* t = mkLabel(scrBoot, &lv_font_montserrat_28, C_TEXT, "PillDispenser");
  lv_obj_align(t, LV_ALIGN_CENTER, 0, -18);
  lv_obj_t* v = mkLabel(scrBoot, &lv_font_montserrat_16, C_BRAND, "by " TEAM_NAME);
  lv_obj_align(v, LV_ALIGN_CENTER, 0, 12);
  lv_obj_t* who = mkLabel(scrBoot, &lv_font_montserrat_12, C_MUTED, TEAM_MEMBERS);
  lv_label_set_long_mode(who, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(who, 290);
  lv_obj_set_style_text_align(who, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(who, LV_ALIGN_BOTTOM_MID, 0, -6);
  lv_obj_t* ver = mkLabel(scrBoot, &lv_font_montserrat_12, C_MUTED, "v" FW_VERSION);
  lv_obj_align(ver, LV_ALIGN_TOP_RIGHT, -8, 6);
  bootBar = lv_bar_create(scrBoot);
  lv_obj_set_size(bootBar, 180, 6);
  lv_obj_align(bootBar, LV_ALIGN_CENTER, 0, 44);
  lv_obj_set_style_bg_color(bootBar, C_CARD2, 0);
  lv_obj_set_style_bg_color(bootBar, C_BRAND, LV_PART_INDICATOR);
  bootMsg = mkLabel(scrBoot, &lv_font_montserrat_12, C_MUTED, "Starting...");
  lv_obj_align(bootMsg, LV_ALIGN_CENTER, 0, 64);
}

void uiBoot(int pct, const char* msg) {
  Serial.printf("[boot] %d %s\n", pct, msg);
  Serial.flush();
  lv_bar_set_value(bootBar, pct, LV_ANIM_OFF);
  lv_label_set_text(bootMsg, msg);
  lv_obj_align(bootMsg, LV_ALIGN_CENTER, 0, 70);
  lv_timer_handler();
}

// ---------------- HOME ----------------
// 320x240:
//   header     clock, date, Wi-Fi, settings
//   next dose  big time, countdown pill, medicine + instructions
//   today      one chip per dose: green taken, red missed/skipped, amber due, grey upcoming
//   bottom     tray stock (tap for tray tools) and a large SOS button (hold 2 s)
static void trayCardCb(lv_event_t*) {
  curTray = 1;
  go(scrTray);
}

static void cardCb(lv_event_t*) {
  AppMode m;
  { Lock l; m = appMode(); }
  if (m == M_SNOOZED) withPin(pinForDispense(), doDispenseDue);
  else if (!g_timeOk) go(scrInfo);
  else trayCardCb(nullptr);
}

static void sosHoldCb(lv_event_t* e) {
  static uint32_t downAt = 0;
  static bool fired = false;
  lv_event_code_t c = lv_event_get_code(e);
  if (c == LV_EVENT_PRESSED) {
    downAt = millis();
    fired = false;
    lv_label_set_text(hSosLbl, "Hold...");
  } else if (c == LV_EVENT_PRESSING && !fired && millis() - downAt > 2000) {
    fired = true;
    postCmd(C_SOS);
  } else if (c == LV_EVENT_RELEASED || c == LV_EVENT_PRESS_LOST) {
    lv_label_set_text(hSosLbl, "SOS");
    if (!fired && c == LV_EVENT_RELEASED) uiToast("Hold SOS for 2 seconds to call your family");
  }
}

static void buildHome() {
  scrHome = mkScreen();
  hTime = mkLabel(scrHome, &lv_font_montserrat_20, C_TEXT, "--:--");
  lv_obj_set_pos(hTime, 14, 10);
  hDate = mkLabel(scrHome, &lv_font_montserrat_14, C_MUTED, "");
  lv_obj_set_pos(hDate, 82, 14);
  hWifi = mkLabel(scrHome, &lv_font_montserrat_16, C_MUTED, LV_SYMBOL_WIFI);
  lv_obj_set_pos(hWifi, 234, 11);
  mkBtn(scrHome, 264, 5, 48, 30, LV_SYMBOL_SETTINGS, C_CARD2, C_TEXT, &lv_font_montserrat_16,
        [](lv_event_t*) { withPin(pinForSettings(), openSettings); }, 15);

  // next dose
  hCard = mkPanel(scrHome, 8, 40, 304, 108, C_CARD, 16);
  lv_obj_add_flag(hCard, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(hCard, cardCb, LV_EVENT_CLICKED, NULL);
  hCardTitle = mkLabel(hCard, &lv_font_montserrat_12, C_MUTED, "NEXT DOSE");
  lv_obj_set_style_text_letter_space(hCardTitle, 1, 0);
  lv_obj_set_pos(hCardTitle, 14, 12);
  hPill = mkPanel(hCard, 0, 0, 10, 24, C_CARD2, 12);
  lv_obj_set_size(hPill, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_pad_hor(hPill, 10, 0);
  lv_obj_set_style_pad_ver(hPill, 4, 0);
  lv_obj_clear_flag(hPill, LV_OBJ_FLAG_CLICKABLE);
  hPillLbl = mkLabel(hPill, &lv_font_montserrat_14, C_TEXT, "");
  lv_obj_align(hPill, LV_ALIGN_TOP_RIGHT, -10, 8);
  hCardBig = mkLabel(hCard, &lv_font_montserrat_48, C_WHITE, "");
  lv_obj_set_pos(hCardBig, 11, 28);
  hDot = mkCircle(hCard, 10, personColor(0));
  lv_obj_set_pos(hDot, 15, 87);
  hCardText = mkLabel(hCard, &lv_font_montserrat_14, C_TEXT, "");
  lv_label_set_long_mode(hCardText, LV_LABEL_LONG_DOT);
  lv_obj_set_width(hCardText, 262);
  lv_obj_set_pos(hCardText, 32, 83);

  // today's doses
  hTodayNone = mkLabel(scrHome, &lv_font_montserrat_14, C_MUTED, "No doses today");
  lv_obj_set_pos(hTodayNone, 14, 162);
  for (int i = 0; i < MAX_SLOTS; i++) {
    lv_obj_t* c = mkPanel(scrHome, 8 + i * 78, 154, 72, 32, C_CARD, 16);
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
    hChip[i] = c;
    hChipLbl[i] = mkLabel(c, &lv_font_montserrat_14, C_MUTED, "");
    lv_obj_center(hChipLbl[i]);
  }

  // the tray
  trCard = mkPanel(scrHome, 8, 192, 214, 42, C_CARD, 12);
  lv_obj_add_flag(trCard, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(trCard, trayCardCb, LV_EVENT_CLICKED, NULL);
  lv_obj_set_style_bg_color(trCard, C_CARD2, LV_STATE_PRESSED);
  trStock = mkLabel(trCard, &lv_font_montserrat_14, C_TEXT, "");
  lv_obj_set_pos(trStock, 12, 5);
  trDays = mkLabel(trCard, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_align(trDays, LV_ALIGN_TOP_RIGHT, -12, 7);
  trBar = lv_bar_create(trCard);
  lv_obj_set_size(trBar, 190, 6);
  lv_obj_set_pos(trBar, 12, 28);
  lv_obj_set_style_bg_color(trBar, C_CARD2, 0);
  lv_obj_set_style_bg_opa(trBar, LV_OPA_COVER, 0);
  lv_obj_clear_flag(trBar, LV_OBJ_FLAG_CLICKABLE);

  // SOS: hold for 2 seconds
  lv_obj_t* sos = mkBtn(scrHome, 228, 192, 84, 42, "SOS", C_RED, C_WHITE, &lv_font_montserrat_20, nullptr, 21);
  hSosLbl = lv_obj_get_child(sos, 0);
  lv_obj_add_event_cb(sos, sosHoldCb, LV_EVENT_ALL, NULL);
}

// state of dose slot s today, for the chip colour / icon
static void slotChip(uint8_t t, uint8_t s, uint32_t todayKey, lv_color_t& col, const char*& icon) {
  const ActiveDose& a = act[t];
  col = C_MUTED;
  icon = "";
  if (a.slot == s && a.st == DS_DUE) { col = C_AMBER; icon = LV_SYMBOL_BELL; return; }
  if (a.slot == s && (a.st == DS_DISPENSING || a.st == DS_READY)) { col = C_BRAND; icon = LV_SYMBOL_DOWNLOAD; return; }
  if (ps.slotEvDay[t][s] == todayKey) {
    switch (ps.slotEv[t][s]) {
      case EV_TAKEN: case EV_EARLY: case EV_DISPENSED: col = C_BRAND; icon = LV_SYMBOL_OK; return;
      case EV_MISSED: case EV_SKIPPED: col = C_RED; icon = LV_SYMBOL_CLOSE; return;
      case EV_UNCONFIRMED: col = C_AMBER; icon = "?"; return;
    }
  }
  if (ps.slotDay[t][s] == todayKey) icon = "-";  // passed before it was scheduled / device was off
}

static void refreshHome() {
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  char b[32];
  if (g_timeOk) {
    strftime(b, sizeof(b), "%H:%M", &lt);
    setText(hTime, b);
    strftime(b, sizeof(b), "%a %d %b", &lt);
    setText(hDate, b);
  } else {
    setText(hTime, "--:--");
    setText(hDate, "Clock not set");
  }
  lv_obj_update_layout(hTime);
  lv_obj_align_to(hDate, hTime, LV_ALIGN_OUT_RIGHT_BOTTOM, 8, -2);
  bool wifi = WiFi.status() == WL_CONNECTED;
  lv_obj_set_style_text_color(hWifi, wifi ? C_BRAND : (apActive() ? C_AMBER : C_RED), 0);

  Lock l;
  const uint8_t t = 1;
  const TrayCfg& tc = cfg.tray[t];
  AppMode m = appMode();
  uint32_t todayKey = dayKey(lt);

  // --- next dose card ---
  lv_color_t cardBg = C_CARD, titleCol = C_MUTED;
  bool bigTime = true;
  String pill;
  if (!g_timeOk) {
    setText(hCardTitle, "SET UP");
    setText(hCardBig, "Set the clock");
    setText(hCardText, apActive() ? "Tap to connect your phone" : "Waiting for internet time...");
    bigTime = false;
  } else if (m == M_SNOOZED) {
    setText(hCardTitle, "SNOOZED  -  TAP TO DISPENSE");
    titleCol = C_AMBER;
    setText(hCardBig, hhmm(alertDueTime()));
    uint32_t until = act[t].snoozeUntil;
    if (until > (uint32_t)now) pill = String(LV_SYMBOL_BELL " ") + relTime(until - now);
    const String& note = act[t].slot < MAX_SLOTS ? tc.slot[act[t].slot].note : tc.name;
    setText(hCardText, note.length() ? tc.name + "  -  " + note : tc.name);
    cardBg = soft(C_AMBER, 50);
  } else {
    uint8_t nt, ns;
    uint32_t when;
    if (nextDose(nt, ns, when)) {
      time_t w = when;
      struct tm wl;
      localtime_r(&w, &wl);
      uint32_t wk = dayKey(wl);
      struct tm tmr = lt;
      tmr.tm_mday += 1;
      tmr.tm_hour = 12;
      tmr.tm_isdst = -1;
      mktime(&tmr);
      if (wk == todayKey) pill = String("in ") + relTime(when - now);
      else if (wk == dayKey(tmr)) pill = "Tomorrow";
      else { strftime(b, sizeof(b), "%A", &wl); pill = b; }
      setText(hCardTitle, "NEXT DOSE");
      setText(hCardBig, hhmm(when));
      const String& note = tc.slot[ns].note;
      setText(hCardText, note.length() ? tc.name + "  -  " + note : tc.name + "  -  " + cfg.people[tc.person]);
    } else {
      setText(hCardTitle, "NEXT DOSE");
      setText(hCardBig, "No times set");
      setText(hCardText, tc.alerts ? "Add dose times in the phone app" : "Reminders are off");
      bigTime = false;
    }
  }
  setText(hPillLbl, pill);
  show(hPill, pill.length() > 0);
  lv_obj_set_style_text_color(hCardTitle, titleCol, 0);
  lv_obj_set_style_text_font(hCardBig, bigTime ? &lv_font_montserrat_48 : &lv_font_montserrat_28, 0);
  lv_obj_set_pos(hCardBig, 12, bigTime ? 28 : 38);
  lv_obj_set_style_bg_color(hCard, cardBg, 0);
  lv_obj_set_style_bg_color(hDot, personColor(tc.person), 0);
  show(hDot, g_timeOk);
  lv_obj_set_pos(hCardText, g_timeOk ? 32 : 14, 83);

  // --- today's doses, in time order ---
  uint8_t order[MAX_SLOTS], n = 0;
  if (g_timeOk && tc.alerts)
    for (int s = 0; s < MAX_SLOTS; s++)
      if (tc.slot[s].on && (tc.slot[s].days & (1 << lt.tm_wday))) order[n++] = s;
  for (int i = 1; i < n; i++)  // insertion sort by time
    for (int j = i; j > 0 && tc.slot[order[j]].h * 60 + tc.slot[order[j]].m <
                                 tc.slot[order[j - 1]].h * 60 + tc.slot[order[j - 1]].m; j--) {
      uint8_t x = order[j]; order[j] = order[j - 1]; order[j - 1] = x;
    }
  int w = n ? (304 - (n - 1) * 6) / n : 0;
  for (int i = 0; i < MAX_SLOTS; i++) {
    if (i >= n) { lv_obj_add_flag(hChip[i], LV_OBJ_FLAG_HIDDEN); continue; }
    uint8_t s = order[i];
    lv_color_t col;
    const char* icon;
    slotChip(t, s, todayKey, col, icon);
    snprintf(b, sizeof(b), "%02u:%02u  %s", tc.slot[s].h, tc.slot[s].m, icon);
    setText(hChipLbl[i], b);
    lv_obj_set_size(hChip[i], w, 32);
    lv_obj_set_pos(hChip[i], 8 + i * (w + 6), 154);
    bool done = col.full != C_MUTED.full;
    lv_obj_set_style_bg_color(hChip[i], done ? soft(col, 64) : C_CARD, 0);
    lv_obj_set_style_text_color(hChipLbl[i], done ? col : C_MUTED, 0);
    lv_obj_center(hChipLbl[i]);
    lv_obj_clear_flag(hChip[i], LV_OBJ_FLAG_HIDDEN);
  }
  if (n) lv_obj_add_flag(hTodayNone, LV_OBJ_FLAG_HIDDEN);
  else {
    setText(hTodayNone, !g_timeOk ? "" : tc.alerts ? "No doses today" : "Reminders off - dispense by hand");
    lv_obj_clear_flag(hTodayNone, LV_OBJ_FLAG_HIDDEN);
  }

  // --- tray ---
  int dl = daysLeft(t);
  int stock = max<int>(0, ps.stock[t]);
  bool empty = stock <= 0, low = !empty && dl >= 0 && dl <= cfg.lowStockDays;
  lv_color_t sc = empty ? C_RED : low ? C_AMBER : C_BRAND;
  lv_bar_set_range(trBar, 0, cfg.capacity);
  lv_bar_set_value(trBar, stock, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(trBar, sc, LV_PART_INDICATOR);
  setText(trStock, empty ? String("Tray is empty") : String(stock) + (stock == 1 ? " dose left" : " doses left"));
  lv_obj_set_style_text_color(trStock, empty ? C_RED : C_TEXT, 0);
  setText(trDays, empty ? String("Refill " LV_SYMBOL_RIGHT) : dl >= 0 ? String(dl) + (dl == 1 ? " day" : " days") : String(""));
  lv_obj_set_style_text_color(trDays, empty ? C_RED : low ? C_AMBER : C_MUTED, 0);
}

// ---------------- ALERT ----------------
// Full amber screen, one big dark DISPENSE button: readable from across the room.
static void buildAlert() {
  scrAlert = mkScreen(C_AMBER);
  lv_obj_set_style_text_color(scrAlert, C_INK, 0);
  aDot = mkCircle(scrAlert, 12, personColor(0));
  lv_obj_set_style_border_color(aDot, C_INK, 0);
  lv_obj_set_style_border_width(aDot, 2, 0);
  lv_obj_set_pos(aDot, 14, 14);
  aWho = mkLabel(scrAlert, &lv_font_montserrat_16, C_INK, "");
  lv_label_set_long_mode(aWho, LV_LABEL_LONG_DOT);
  lv_obj_set_width(aWho, 170);
  lv_obj_set_pos(aWho, 33, 11);
  aDue = mkLabel(scrAlert, &lv_font_montserrat_14, C_INK, "");
  lv_obj_align(aDue, LV_ALIGN_TOP_RIGHT, -14, 13);
  lv_obj_t* t = mkLabel(scrAlert, &lv_font_montserrat_28, C_INK, "Time for your pills");
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 36);
  aTrays = mkLabel(scrAlert, &lv_font_montserrat_16, C_INK, "");
  lv_label_set_long_mode(aTrays, LV_LABEL_LONG_DOT);
  lv_obj_set_width(aTrays, 296);
  lv_obj_set_style_text_align(aTrays, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(aTrays, LV_ALIGN_TOP_MID, 0, 72);
  aTime = mkLabel(scrAlert, &lv_font_montserrat_14, lv_color_mix(C_INK, C_AMBER, 200), "");
  lv_label_set_long_mode(aTime, LV_LABEL_LONG_DOT);
  lv_obj_set_width(aTime, 296);
  lv_obj_set_style_text_align(aTime, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(aTime, LV_ALIGN_TOP_MID, 0, 93);
  lv_obj_t* d = mkBtn(scrAlert, 18, 118, 284, 62, LV_SYMBOL_DOWNLOAD "  DISPENSE", C_INK, C_WHITE, &lv_font_montserrat_28,
                      [](lv_event_t*) { withPin(pinForDispense(), doDispenseDue); }, 31);
  pulse(d, 6, 900);
  lv_obj_t* sn = mkBtn(scrAlert, 18, 190, 138, 44, "", C_INK, C_INK, &lv_font_montserrat_16, [](lv_event_t*) {
    postCmd(C_SNOOZE);
    go(scrHome);
  }, 22);
  lv_obj_set_style_bg_opa(sn, LV_OPA_20, 0);
  aSnooze = lv_obj_get_child(sn, 0);
  lv_obj_t* sk = mkBtn(scrAlert, 164, 190, 138, 44, LV_SYMBOL_CLOSE "  Skip", C_INK, C_INK, &lv_font_montserrat_16, [](lv_event_t*) {
    confirmBox("Skip this dose?", "The pills stay in the tray and the dose is recorded as skipped.", "Skip", doSkip);
  }, 22);
  lv_obj_set_style_bg_opa(sk, LV_OPA_20, 0);
}

static void refreshAlert() {
  Lock l;
  int8_t p = alertPerson();
  bool demo = p < 0 && previewing();
  if (p < 0 && !demo) return;
  if (demo) p = cfg.tray[1].person;
  lv_obj_set_style_bg_color(aDot, personColor(p), 0);
  setText(aWho, cfg.people[p]);
  String trays, note;
  uint32_t due = 0;
  for (int t = 1; t <= MAX_TRAYS; t++) {
    if (act[t].st != DS_DUE || cfg.tray[t].person != p) continue;
    if (trays.length()) trays += " + ";
    trays += cfg.tray[t].name;
    if (act[t].slot < MAX_SLOTS && !note.length()) note = cfg.tray[t].slot[act[t].slot].note;
    if (!due || act[t].due < due) due = act[t].due;
  }
  if (demo) { trays = cfg.tray[1].name; note = cfg.tray[1].slot[0].note; due = time(nullptr) - 420; }
  setText(aTrays, trays);
  int32_t late = (int32_t)time(nullptr) - (int32_t)due;
  setText(aDue, String("Due ") + hhmm(due));
  lv_obj_align(aDue, LV_ALIGN_TOP_RIGHT, -14, 13);
  String sub = note;
  if (late >= 60) sub += (sub.length() ? "   " : "") + relTime(late) + " late";
  setText(aTime, sub);
  setText(aSnooze, String(LV_SYMBOL_BELL "  ") + cfg.snoozeMin + " min");
}

// ---------------- BUSY ----------------
static void buildBusy() {
  scrBusy = mkScreen();
  lv_obj_t* sp = lv_spinner_create(scrBusy, 1000, 60);
  lv_obj_set_size(sp, 84, 84);
  lv_obj_align(sp, LV_ALIGN_CENTER, 0, -36);
  lv_obj_set_style_arc_color(sp, C_BRAND, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(sp, C_CARD2, LV_PART_MAIN);
  lv_obj_set_style_arc_width(sp, 8, LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(sp, 8, LV_PART_MAIN);
  lv_obj_t* t = mkLabel(scrBusy, &lv_font_montserrat_28, C_TEXT, "Dispensing...");
  lv_obj_align(t, LV_ALIGN_CENTER, 0, 34);
  bWho = mkLabel(scrBusy, &lv_font_montserrat_16, C_MUTED, "");
  lv_obj_align(bWho, LV_ALIGN_CENTER, 0, 68);
}

// ---------------- READY ----------------
static void readyBtnCb(lv_event_t*) {
  bool manual;
  int8_t p;
  { Lock l; manual = readyIsManual; p = readyPerson; }
  postCmd(C_TAKEN, SRC_DEVICE, manual ? -2 : p);  // -2: just close a manual "pills ready" screen
  go(scrHome);
}

static void buildReady() {
  scrReady = mkScreen(C_READY);
  lv_obj_t* ring = mkCircle(scrReady, 56, C_WHITE);
  lv_obj_align(ring, LV_ALIGN_TOP_MID, 0, 14);
  lv_obj_t* ck = mkLabel(ring, &lv_font_montserrat_28, C_READY, LV_SYMBOL_OK);
  lv_obj_center(ck);
  lv_obj_t* t = mkLabel(scrReady, &lv_font_montserrat_28, C_WHITE, "Pills are ready");
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 78);
  rWho = mkLabel(scrReady, &lv_font_montserrat_16, C_WHITE, "");
  lv_label_set_long_mode(rWho, LV_LABEL_LONG_DOT);
  lv_obj_set_width(rWho, 296);
  lv_obj_set_style_text_align(rWho, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(rWho, LV_ALIGN_TOP_MID, 0, 112);
  rHint = mkLabel(scrReady, &lv_font_montserrat_14, lv_color_mix(C_WHITE, C_READY, 200), "");
  lv_obj_set_width(rHint, 296);
  lv_obj_set_style_text_align(rHint, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(rHint, LV_ALIGN_TOP_MID, 0, 134);
  lv_obj_t* b = mkBtn(scrReady, 18, 164, 284, 62, "", C_WHITE, C_READY, &lv_font_montserrat_28, readyBtnCb, 31);
  rBtnLbl = lv_obj_get_child(b, 0);
}

static void refreshReady() {
  Lock l;
  int8_t p = readyPerson;
  bool demo = p < 0 && previewing();
  if (p < 0 && !demo) return;
  if (demo) p = cfg.tray[1].person;
  setText(rWho, cfg.people[p] + "  -  " + cfg.tray[1].name);
  if (readyIsManual && !demo) {
    setText(rHint, "Take them from the cup.");
    setText(rBtnLbl, LV_SYMBOL_OK "  Done");
  } else {
    setText(rHint, "Take them from the cup, then tap below");
    setText(rBtnLbl, LV_SYMBOL_OK "  I took them");
  }
}

// ---------------- header for sub screens ----------------
static lv_obj_t* mkHeader(lv_obj_t* scr, const char* title, lv_event_cb_t back) {
  mkBtn(scr, 8, 6, 36, 32, LV_SYMBOL_LEFT, C_CARD2, C_TEXT, &lv_font_montserrat_16, back, 16);
  lv_obj_t* t = mkLabel(scr, &lv_font_montserrat_20, C_TEXT, title);
  lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
  lv_obj_set_width(t, 254);
  lv_obj_set_pos(t, 56, 11);
  return t;
}
static void backHome(lv_event_t*) { go(scrHome); }

// ---------------- TRAY ----------------
static void refillStart();
static void buildTray() {
  scrTray = mkScreen();
  tTitle = mkHeader(scrTray, "", backHome);
  tPersonDot = mkCircle(scrTray, 12, C_BRAND);
  lv_obj_set_pos(tPersonDot, 14, 52);
  tPerson = mkLabel(scrTray, &lv_font_montserrat_16, C_TEXT, "");
  lv_obj_set_pos(tPerson, 34, 48);
  tSched = mkLabel(scrTray, &lv_font_montserrat_12, C_MUTED, "");
  lv_label_set_long_mode(tSched, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(tSched, 296);
  lv_obj_set_pos(tSched, 14, 70);
  tBar = lv_bar_create(scrTray);
  lv_obj_set_size(tBar, 292, 8);
  lv_obj_set_pos(tBar, 14, 102);
  lv_obj_set_style_bg_color(tBar, C_CARD2, 0);
  lv_obj_set_style_bg_opa(tBar, LV_OPA_COVER, 0);
  tStock = mkLabel(scrTray, &lv_font_montserrat_14, C_TEXT, "");
  lv_obj_set_pos(tStock, 14, 116);
  tLast = mkLabel(scrTray, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_set_pos(tLast, 14, 136);

  mkBtn(scrTray, 8, 156, 148, 40, LV_SYMBOL_DOWNLOAD "  Dispense", C_PRI, C_WHITE, &lv_font_montserrat_16, [](lv_event_t*) {
    withPin(pinForDispense(), [] {
      const char* why;
      { Lock l; why = manualCheck(curTray); }
      if (!why) { doManual(); return; }
      if (!strncmp(why, "Safety", 6) || !strncmp(why, "Tray is empty", 13))
        confirmBox("Are you sure?", why, "Dispense", doManualForce);
      else uiToast(why);
    });
  });
  mkBtn(scrTray, 164, 156, 148, 40, LV_SYMBOL_REFRESH "  Refill", C_CARD2, C_TEXT, &lv_font_montserrat_16,
        [](lv_event_t*) { refillStart(); });
  mkBtn(scrTray, 8, 200, 148, 34, LV_SYMBOL_NEXT "  Next slot", C_CARD, C_TEXT, &lv_font_montserrat_14, [](lv_event_t*) {
    withPin(pinForSettings(), [] { postCmd(C_ADVANCE, SRC_DEVICE, curTray, 1); });
  });
  mkBtn(scrTray, 164, 200, 148, 34, LV_SYMBOL_LOOP "  Align half", C_CARD, C_TEXT, &lv_font_montserrat_14, [](lv_event_t*) {
    withPin(pinForSettings(), [] { postCmd(C_ALIGN, SRC_DEVICE, curTray); });
  });
}

static void refreshTray() {
  Lock l;
  uint8_t t = curTray;
  const TrayCfg& tc = cfg.tray[t];
  setText(tTitle, tc.name);
  lv_obj_set_style_bg_color(tPersonDot, personColor(tc.person), 0);
  setText(tPerson, cfg.people[tc.person]);
  setText(tSched, scheduleText(t));
  lv_bar_set_range(tBar, 0, cfg.capacity);
  lv_bar_set_value(tBar, max<int>(0, ps.stock[t]), LV_ANIM_OFF);
  int dl = daysLeft(t);
  lv_color_t bc = ps.stock[t] <= 0 ? C_RED : (dl >= 0 && dl <= cfg.lowStockDays ? C_AMBER : C_BRAND);
  lv_obj_set_style_bg_color(tBar, bc, LV_PART_INDICATOR);
  setText(tStock, String(max<int>(0, ps.stock[t])) + " of " + cfg.capacity + " doses left" + (dl >= 0 ? String("  -  about ") + dl + (dl == 1 ? " day" : " days") : String("")));
  String last = "Never dispensed";
  if (ps.lastDisp[t]) {
    time_t e = ps.lastDisp[t];
    struct tm el;
    localtime_r(&e, &el);
    char b[24];
    strftime(b, sizeof(b), "%a %d %b %H:%M", &el);
    last = String("Last dispensed ") + b;
  }
  setText(tLast, last);
}

// ---------------- SOS ----------------
static lv_obj_t *sosTitle, *sosSub;
static void buildSos() {
  scrSos = mkScreen(C_SOSBG);
  lv_obj_t* ring = mkCircle(scrSos, 80, C_WHITE);
  lv_obj_align(ring, LV_ALIGN_TOP_MID, 0, 14);
  lv_obj_t* t = mkLabel(ring, &lv_font_montserrat_28, C_SOSBG, "SOS");
  lv_obj_center(t);
  pulse(ring, 10, 700);
  sosTitle = mkLabel(scrSos, &lv_font_montserrat_20, C_WHITE, "");
  lv_obj_set_width(sosTitle, 300);
  lv_obj_set_style_text_align(sosTitle, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(sosTitle, LV_ALIGN_TOP_MID, 0, 108);
  sosSub = mkLabel(scrSos, &lv_font_montserrat_14, lv_color_mix(C_WHITE, C_SOSBG, 210), "");
  lv_obj_set_width(sosSub, 300);
  lv_obj_set_style_text_align(sosSub, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(sosSub, LV_ALIGN_TOP_MID, 0, 136);
  mkBtn(scrSos, 18, 172, 284, 56, LV_SYMBOL_OK "  I'm OK - cancel", C_WHITE, C_INK, &lv_font_montserrat_20,
        [](lv_event_t*) { postCmd(C_SOS_CANCEL); go(scrHome); }, 28);
}

static void refreshSos() {
  Lock l;
  if (!g_sos.on && !previewing()) return;
  if (g_sos.ack.length()) {
    setText(sosTitle, g_sos.ack + " is on the way");
    setText(sosSub, "Stay where you are. Help is coming.");
  } else {
    setText(sosTitle, "Calling your family...");
    setText(sosSub, String("Alert sent") + (g_timeOk && g_sos.at ? " at " + hhmm(g_sos.at) : String("")) + ". Help is on the way.");
  }
  lv_obj_align(sosTitle, LV_ALIGN_TOP_MID, 0, 108);
  lv_obj_align(sosSub, LV_ALIGN_TOP_MID, 0, 136);
}

// ---------------- PARTY MODE ----------------
static lv_obj_t *pDisc, *pTitle, *pSub, *pNote;
static void buildParty() {
  scrParty = mkScreen(lv_color_hex(0x6A1BD8));
  pDisc = mkCircle(scrParty, 96, C_WHITE);
  lv_obj_align(pDisc, LV_ALIGN_TOP_MID, 0, 12);
  pNote = mkLabel(pDisc, &lv_font_montserrat_48, C_INK, LV_SYMBOL_AUDIO);
  lv_obj_center(pNote);
  pulse(pDisc, 14, 260);
  pTitle = mkLabel(scrParty, &lv_font_montserrat_28, C_WHITE, "Party mode");
  lv_obj_align(pTitle, LV_ALIGN_TOP_MID, 0, 116);
  pSub = mkLabel(scrParty, &lv_font_montserrat_14, C_WHITE, "Medicine alarms still come first");
  lv_obj_align(pSub, LV_ALIGN_TOP_MID, 0, 152);
  mkBtn(scrParty, 18, 176, 284, 54, LV_SYMBOL_STOP "  Stop", C_WHITE, C_INK, &lv_font_montserrat_20,
        [](lv_event_t*) { postCmd(C_PARTY, SRC_DEVICE, 0); go(scrHome); }, 27);
}

// Called every 150 ms. Party: the background walks round the colour wheel. Shabad: calm, steady saffron.
static void refreshParty() {
  static uint8_t shown = 0xFF;
  uint8_t k = previewing() ? MUSIC_PARTY : g_party;
  if (k != shown) {
    shown = k;
    bool sh = k == MUSIC_SHABAD;
    setText(pTitle, sh ? "Shabad Gurbani" : "Party mode");
    setText(pSub, sh ? "Waheguru Ji  -  medicine alarms still come first" : "Medicine alarms still come first");
    setText(pNote, LV_SYMBOL_AUDIO);
    lv_obj_align(pTitle, LV_ALIGN_TOP_MID, 0, 116);
    lv_obj_align(pSub, LV_ALIGN_TOP_MID, 0, 152);
    lv_anim_del(pDisc, nullptr);
    lv_obj_set_style_transform_width(pDisc, 0, 0);
    lv_obj_set_style_transform_height(pDisc, 0, 0);
    if (sh) {
      lv_obj_set_style_bg_color(scrParty, lv_color_hex(0xC7650A), 0);
      pulse(pDisc, 6, 2000);  // slow, like breathing
    } else {
      pulse(pDisc, 14, 260);  // on the beat
    }
  }
  if (k == MUSIC_SHABAD) return;
  static uint16_t hue = 270;
  hue = (hue + 23) % 360;
  lv_obj_set_style_bg_color(scrParty, lv_color_hsv_to_rgb(hue, 85, 80), 0);
}

// ---------------- FAMILY MESSAGE ----------------
static lv_obj_t *mFrom, *mWhen, *mText;
static void buildMsg() {
  scrMsg = mkScreen();
  mFrom = mkLabel(scrMsg, &lv_font_montserrat_16, C_BRAND, "");
  lv_label_set_long_mode(mFrom, LV_LABEL_LONG_DOT);
  lv_obj_set_width(mFrom, 230);
  lv_obj_set_pos(mFrom, 14, 12);
  mWhen = mkLabel(scrMsg, &lv_font_montserrat_14, C_MUTED, "");
  lv_obj_align(mWhen, LV_ALIGN_TOP_RIGHT, -14, 13);
  lv_obj_t* card = mkPanel(scrMsg, 10, 40, 300, 134, C_PAPER, 14);
  mText = mkLabel(card, &lv_font_montserrat_20, C_INK, "");
  lv_label_set_long_mode(mText, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(mText, 276);
  lv_obj_set_pos(mText, 12, 11);
  mkBtn(scrMsg, 18, 182, 284, 50, LV_SYMBOL_OK "  OK, got it", C_PRI, C_WHITE, &lv_font_montserrat_20,
        [](lv_event_t*) { postCmd(C_MSG_ACK); go(scrHome); }, 25);
}

// the screen font only has basic Latin: drop emoji / other scripts instead of showing boxes
static String screenText(const String& in) {
  String o;
  for (unsigned char c : in) if ((c >= 32 && c < 127) || c == 10) o += (char)c;
  o.trim();
  while (o.indexOf("  ") >= 0) o.replace("  ", " ");
  return o.length() ? o : String("(This message uses characters the screen can't show - please check Telegram.)");
}

static void refreshMsg() {
  Lock l;
  bool demo = !g_msg.text.length() && previewing();
  setText(mFrom, String(LV_SYMBOL_ENVELOPE "  From ") + (demo ? String("Ahmed") : g_msg.from));
  setText(mWhen, demo ? hhmm(time(nullptr)) : g_timeOk && g_msg.at ? hhmm(g_msg.at) : String(""));
  lv_obj_align(mWhen, LV_ALIGN_TOP_RIGHT, -14, 13);
  String t = demo ? String("I will visit you at 6 pm. Please keep the door open.") : screenText(g_msg.text);
  lv_obj_set_style_text_font(mText, t.length() > 90 ? &lv_font_montserrat_16 : &lv_font_montserrat_20, 0);
  setText(mText, t);
}

// ---------------- REFILL WIZARD ----------------
static lv_obj_t *fStep, *fBody, *fCount, *fMinus, *fPlus, *fLeft, *fLeftLbl, *fRightLbl;
static uint8_t refStep = 1;
static int refCount = 30;

static void refillShow() {
  uint8_t cap;
  { Lock l; cap = cfg.capacity; }
  setText(fStep, String("STEP ") + refStep + " OF 3");
  bool pick = refStep == 3;
  if (refStep == 1)
    setText(fBody, String("Open the lid and put one dose into each compartment. The tray holds ") + cap + " doses.");
  else if (refStep == 2)
    setText(fBody, "Close the lid. Tap Align until the empty compartment is over the cup opening.");
  else
    setText(fBody, "How many doses did you put in?");
  setText(fCount, String(refCount));
  lv_obj_align(fCount, LV_ALIGN_TOP_MID, 0, 114);
  for (lv_obj_t* o : {fCount, fMinus, fPlus}) show(o, pick);
  setText(fLeftLbl, refStep == 2 ? LV_SYMBOL_LOOP "  Align" : LV_SYMBOL_LEFT "  Back");
  setText(fRightLbl, pick ? LV_SYMBOL_OK "  Done" : "Next  " LV_SYMBOL_RIGHT);
}

static void refillStart() {
  { Lock l; refCount = cfg.capacity; }
  refStep = 1;
  refillShow();
  go(scrRefill);
}

static void buildRefill() {
  scrRefill = mkScreen();
  mkHeader(scrRefill, "Refill the tray", [](lv_event_t*) { go(scrTray); });
  fStep = mkLabel(scrRefill, &lv_font_montserrat_12, C_BRAND, "");
  lv_obj_set_style_text_letter_space(fStep, 1, 0);
  lv_obj_set_pos(fStep, 14, 48);
  fBody = mkLabel(scrRefill, &lv_font_montserrat_16, C_TEXT, "");
  lv_label_set_long_mode(fBody, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(fBody, 292);
  lv_obj_set_pos(fBody, 14, 68);
  fMinus = mkBtn(scrRefill, 40, 112, 60, 56, LV_SYMBOL_MINUS, C_CARD2, C_TEXT, &lv_font_montserrat_20, [](lv_event_t*) {
    if (refCount > 0) refCount--;
    refillShow();
  }, 28);
  fPlus = mkBtn(scrRefill, 220, 112, 60, 56, LV_SYMBOL_PLUS, C_CARD2, C_TEXT, &lv_font_montserrat_20, [](lv_event_t*) {
    uint8_t cap;
    { Lock l; cap = cfg.capacity; }
    if (refCount < cap) refCount++;
    refillShow();
  }, 28);
  fCount = mkLabel(scrRefill, &lv_font_montserrat_48, C_WHITE, "");
  fLeft = mkBtn(scrRefill, 8, 186, 148, 46, "", C_CARD2, C_TEXT, &lv_font_montserrat_16, [](lv_event_t*) {
    if (refStep == 2) { withPin(pinForSettings(), [] { postCmd(C_ALIGN, SRC_DEVICE, 1); }); return; }
    if (refStep > 1) refStep--;
    else { go(scrTray); return; }
    refillShow();
  });
  fLeftLbl = lv_obj_get_child(fLeft, 0);
  lv_obj_t* right = mkBtn(scrRefill, 164, 186, 148, 46, "", C_PRI, C_WHITE, &lv_font_montserrat_16, [](lv_event_t*) {
    if (refStep < 3) { refStep++; refillShow(); return; }
    postCmd(C_REFILL, SRC_DEVICE, 1, max(1, refCount));
    go(scrTray);
  });
  fRightLbl = lv_obj_get_child(right, 0);
}

// ---------------- SETTINGS ----------------
static void saveDevice() {
  String j = String("{\"volume\":") + lv_slider_get_value(sVol) + ",\"brightness\":" + lv_slider_get_value(sBright) +
             ",\"mute\":" + (lv_obj_has_state(sMute, LV_STATE_CHECKED) ? "true" : "false") +
             ",\"clockSaver\":" + (lv_obj_has_state(sSaver, LV_STATE_CHECKED) ? "true" : "false") +
             ",\"invert\":" + (lv_obj_has_state(sInvert, LV_STATE_CHECKED) ? "true" : "false") + "}";
  postCmd(C_CONFIG, SRC_DEVICE, 0, 0, j.c_str());
}

static lv_obj_t* settingRow(lv_obj_t* parent, const char* text) {
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_set_size(row, lv_pct(100), 46);
  lv_obj_set_style_bg_color(row, C_CARD, 0);
  lv_obj_set_style_border_width(row, 0, 0);
  lv_obj_set_style_radius(row, 12, 0);
  lv_obj_set_style_pad_hor(row, 12, 0);
  lv_obj_set_style_pad_ver(row, 0, 0);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t* l = mkLabel(row, &lv_font_montserrat_14, C_TEXT, text);
  lv_obj_align(l, LV_ALIGN_LEFT_MID, 0, 0);
  return row;
}

static lv_obj_t* rowSwitch(lv_obj_t* parent, const char* text) {
  lv_obj_t* row = settingRow(parent, text);
  lv_obj_t* sw = lv_switch_create(row);
  lv_obj_set_size(sw, 46, 26);
  lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
  lv_obj_set_style_bg_color(sw, C_CARD2, 0);
  lv_obj_set_style_bg_color(sw, C_PRI, LV_PART_INDICATOR | LV_STATE_CHECKED);
  return sw;
}

static lv_obj_t* rowSlider(lv_obj_t* parent, const char* text, int lo, int hi, int w) {
  lv_obj_t* row = settingRow(parent, text);
  lv_obj_t* s = lv_slider_create(row);
  lv_slider_set_range(s, lo, hi);
  lv_obj_set_width(s, w);
  lv_obj_align(s, LV_ALIGN_RIGHT_MID, w < 150 ? -52 : -10, 0);
  lv_obj_set_style_bg_color(s, C_CARD2, 0);
  lv_obj_set_style_bg_color(s, C_BRAND, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(s, C_WHITE, LV_PART_KNOB);
  return s;
}

static void rowButton(lv_obj_t* parent, const char* text, lv_event_cb_t cb) {
  lv_obj_t* row = settingRow(parent, text);
  lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_bg_color(row, C_CARD2, LV_STATE_PRESSED);
  lv_obj_add_event_cb(row, cb, LV_EVENT_CLICKED, NULL);
  lv_obj_t* a = mkLabel(row, &lv_font_montserrat_14, C_MUTED, LV_SYMBOL_RIGHT);
  lv_obj_align(a, LV_ALIGN_RIGHT_MID, 0, 0);
}

static void buildSettings() {
  scrSettings = mkScreen();
  mkHeader(scrSettings, "Settings", [](lv_event_t*) { saveDevice(); go(scrHome); });
  lv_obj_t* list = lv_obj_create(scrSettings);
  lv_obj_set_pos(list, 0, 44);
  lv_obj_set_size(list, 320, 196);
  lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(list, 0, 0);
  lv_obj_set_style_pad_hor(list, 8, 0);
  lv_obj_set_style_pad_ver(list, 4, 0);
  lv_obj_set_style_pad_row(list, 6, 0);
  lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

  sVol = rowSlider(list, "Volume", 0, 30, 116);
  lv_obj_t* test = mkBtn(lv_obj_get_parent(sVol), 0, 0, 40, 32, LV_SYMBOL_PLAY, C_CARD2, C_TEXT, &lv_font_montserrat_14,
                         [](lv_event_t*) { saveDevice(); postCmd(C_SOUND, SRC_DEVICE, TRACK_READY); }, 16);
  lv_obj_align(test, LV_ALIGN_RIGHT_MID, 0, 0);
  sBright = rowSlider(list, "Brightness", 5, 100, 150);
  lv_obj_add_event_cb(sBright, [](lv_event_t* e) { backlight(lv_slider_get_value(lv_event_get_target(e))); },
                      LV_EVENT_VALUE_CHANGED, NULL);
  sMute = rowSwitch(list, "Mute sounds");
  sSaver = rowSwitch(list, "Show a clock when idle");
  sInvert = rowSwitch(list, "Invert colours (INV screen)");
  lv_obj_add_event_cb(sInvert, [](lv_event_t* e) {
    tft.invertDisplay(!lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
  }, LV_EVENT_VALUE_CHANGED, NULL);
  rowButton(list, LV_SYMBOL_BELL "   Test dose in 1 minute", [](lv_event_t*) { saveDevice(); postCmd(C_DEMO, SRC_DEVICE, 60, 0); go(scrHome); });
  rowButton(list, LV_SYMBOL_AUDIO "   Party mode", [](lv_event_t*) { saveDevice(); postCmd(C_PARTY, SRC_DEVICE, MUSIC_PARTY); });
  rowButton(list, LV_SYMBOL_AUDIO "   Shabad Gurbani", [](lv_event_t*) { saveDevice(); postCmd(C_PARTY, SRC_DEVICE, MUSIC_SHABAD); });
  rowButton(list, LV_SYMBOL_WIFI "   Wi-Fi network", [](lv_event_t*) { go(scrWifi); });
  rowButton(list, LV_SYMBOL_EYE_OPEN "   Phone app & device info", [](lv_event_t*) { go(scrInfo); });
  rowButton(list, LV_SYMBOL_GPS "   Identify / test lights", [](lv_event_t*) { postCmd(C_IDENTIFY); });
  rowButton(list, LV_SYMBOL_POWER "   Restart", [](lv_event_t*) {
    confirmBox("Restart?", "The dispenser will restart. Settings are kept.", "Restart", [] { saveDevice(); postCmd(C_REBOOT); });
  });
}

static void refreshSettingsControls() {
  Lock l;
  lv_slider_set_value(sVol, cfg.volume, LV_ANIM_OFF);
  lv_slider_set_value(sBright, cfg.brightness, LV_ANIM_OFF);
  if (cfg.mute) lv_obj_add_state(sMute, LV_STATE_CHECKED); else lv_obj_clear_state(sMute, LV_STATE_CHECKED);
  if (cfg.clockSaver) lv_obj_add_state(sSaver, LV_STATE_CHECKED); else lv_obj_clear_state(sSaver, LV_STATE_CHECKED);
  if (cfg.invert) lv_obj_add_state(sInvert, LV_STATE_CHECKED); else lv_obj_clear_state(sInvert, LV_STATE_CHECKED);
}

// ---------------- WIFI ----------------
static void styleField(lv_obj_t* o) {
  lv_obj_set_style_bg_color(o, C_CARD, 0);
  lv_obj_set_style_border_color(o, C_LINE, 0);
  lv_obj_set_style_border_width(o, 1, 0);
  lv_obj_set_style_radius(o, 10, 0);
  lv_obj_set_style_text_color(o, C_TEXT, 0);
  lv_obj_set_style_border_color(o, C_BRAND, LV_STATE_FOCUSED);
}

static void wifiConnect() {
  char ssid[64];
  lv_dropdown_get_selected_str(wDrop, ssid, sizeof(ssid));
  if (!ssid[0] || !strcmp(ssid, "Tap Scan first")) { setText(wStatus, "Scan and pick a network first"); return; }
  String s = String(ssid) + "\n" + lv_textarea_get_text(wPass);
  postCmd(C_WIFI, SRC_DEVICE, 0, 0, s.c_str());
  lv_obj_add_flag(wKb, LV_OBJ_FLAG_HIDDEN);
  setText(wStatus, String("Connecting to ") + ssid + "...");
}

static void buildWifi() {
  scrWifi = mkScreen();
  mkHeader(scrWifi, "Wi-Fi", [](lv_event_t*) { go(scrSettings); });
  wDrop = lv_dropdown_create(scrWifi);
  lv_dropdown_set_options(wDrop, "Tap Scan first");
  lv_obj_set_pos(wDrop, 8, 46);
  lv_obj_set_size(wDrop, 214, 40);
  styleField(wDrop);
  mkBtn(scrWifi, 228, 46, 84, 40, LV_SYMBOL_REFRESH " Scan", C_CARD2, C_TEXT, &lv_font_montserrat_14, [](lv_event_t*) {
    if (NET_OFF) { setText(wStatus, g_safeMode ? "Safe mode: Wi-Fi is off. Restart to retry." : "Wi-Fi is off in this build"); return; }
    scanStart();
    wScanning = true;
    setText(wStatus, "Scanning...");
  });
  wPass = lv_textarea_create(scrWifi);
  lv_textarea_set_one_line(wPass, true);
  lv_textarea_set_password_mode(wPass, true);
  lv_textarea_set_placeholder_text(wPass, "Password");
  lv_obj_set_pos(wPass, 8, 92);
  lv_obj_set_size(wPass, 214, 40);
  styleField(wPass);
  mkBtn(scrWifi, 228, 92, 84, 40, "Connect", C_PRI, C_WHITE, &lv_font_montserrat_14, [](lv_event_t*) { wifiConnect(); });
  wStatus = mkLabel(scrWifi, &lv_font_montserrat_12, C_MUTED, "");
  lv_obj_set_width(wStatus, 300);
  lv_label_set_long_mode(wStatus, LV_LABEL_LONG_WRAP);
  lv_obj_set_pos(wStatus, 12, 140);
  mkBtn(scrWifi, 8, 194, 304, 40, LV_SYMBOL_WIFI "  Set up with your phone instead", C_CARD2, C_TEXT, &lv_font_montserrat_14,
        [](lv_event_t*) { postCmd(C_AP); go(scrInfo); });
  wKb = lv_keyboard_create(scrWifi);
  lv_keyboard_set_textarea(wKb, wPass);
  lv_obj_add_flag(wKb, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_event_cb(wPass, [](lv_event_t*) { lv_obj_clear_flag(wKb, LV_OBJ_FLAG_HIDDEN); }, LV_EVENT_CLICKED, NULL);
  lv_obj_add_event_cb(wKb, [](lv_event_t* e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY) wifiConnect();
    if (c == LV_EVENT_CANCEL) lv_obj_add_flag(wKb, LV_OBJ_FLAG_HIDDEN);
  }, LV_EVENT_ALL, NULL);
}

static void refreshWifi() {
  if (wScanning) {
    int n = WiFi.scanComplete();
    if (n >= 0) {
      wScanning = false;
      String opts;
      for (int i = 0; i < n; i++) {
        String s = WiFi.SSID(i);
        if (!s.length() || opts.indexOf(s + "\n") >= 0) continue;
        opts += s + "\n";
      }
      WiFi.scanDelete();
      if (opts.length()) opts.remove(opts.length() - 1);
      lv_dropdown_set_options(wDrop, opts.length() ? opts.c_str() : "Tap Scan first");
      setText(wStatus, String(n) + " networks found. Pick one and type the password.");
    } else if (n == WIFI_SCAN_FAILED) {
      static uint8_t retries = 0;
      if (++retries <= 2) { scanStart(); return; }  // usually a reconnect attempt was in the way
      retries = 0;
      wScanning = false;
      setText(wStatus, "Scan failed, try again");
    }
    return;
  }
  if (WiFi.status() == WL_CONNECTED && strncmp(lv_label_get_text(wStatus), "Connecting", 10) == 0)
    setText(wStatus, String(LV_SYMBOL_OK " Connected to ") + WiFi.SSID() + "  -  " + WiFi.localIP().toString());
}

// ---------------- INFO / QR ----------------
static void buildInfo() {
  scrInfo = mkScreen();
  iTitle = mkHeader(scrInfo, "Phone app", backHome);
  lv_obj_t* qbg = mkPanel(scrInfo, 8, 46, 140, 140, C_WHITE, 12);
  iQr = lv_qrcode_create(qbg, 124, C_INK, C_WHITE);
  lv_obj_center(iQr);
  lv_obj_add_flag(qbg, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(qbg, [](lv_event_t*) { iShowJoin = !iShowJoin; refreshAll(); }, LV_EVENT_CLICKED, NULL);
  iText = mkLabel(scrInfo, &lv_font_montserrat_12, C_TEXT, "");
  lv_label_set_long_mode(iText, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(iText, 156);
  lv_obj_set_pos(iText, 158, 48);
}

static String lastQr;
static void refreshInfo() {
  String ip = localIp(), qr, txt;
  bool ap = apActive();
  if (ap && iShowJoin) {
    qr = "WIFI:T:WPA;S:" + apSsid() + ";P:" + apPass() + ";;";
    txt = "1. Scan with your phone camera to join\n   " + apSsid() + "\n   password " + apPass() +
          "\n\n2. The setup page opens\n   (or go to 192.168.4.1)\n\nTap the code for the web link";
    setText(iTitle, "Connect your phone");
  } else {
    qr = "http://" + ip + "/";
    txt = "Scan to open the phone app\n\nhttp://" + ip + "\nhttp://" + cfg.hostname + ".local\n\n";
    if (WiFi.status() == WL_CONNECTED) txt += "Wi-Fi " + WiFi.SSID() + " (" + WiFi.RSSI() + " dBm)\n";
    txt += String("Version " FW_VERSION "\nUp ") + relTime(millis() / 1000) + "  -  " + ESP.getFreeHeap() / 1024 + " KB free";
    txt += "\nMade by " TEAM_NAME;
    setText(iTitle, ap ? "Connect your phone" : "Phone app");
  }
  if (qr != lastQr) {
    lastQr = qr;
    lv_qrcode_update(iQr, qr.c_str(), qr.length());
  }
  if (g_safeMode) txt = String("SAFE MODE: Wi-Fi is off after repeated crashes. Restart to try again.\n\n") + txt;
  setText(iText, txt);
}

// ---------------- PIN ----------------
static const char* PIN_MAP[] = {"1", "2", "3", "\n", "4", "5", "6", "\n", "7", "8", "9", "\n",
                                LV_SYMBOL_CLOSE, "0", LV_SYMBOL_OK, ""};

static void buildPin() {
  scrPin = mkScreen();
  pMsg = mkLabel(scrPin, &lv_font_montserrat_16, C_TEXT, "Enter PIN");
  lv_obj_set_pos(pMsg, 14, 14);
  pTa = lv_textarea_create(scrPin);
  lv_textarea_set_one_line(pTa, true);
  lv_textarea_set_password_mode(pTa, true);
  lv_textarea_set_max_length(pTa, 6);
  lv_obj_set_pos(pTa, 150, 6);
  lv_obj_set_size(pTa, 162, 38);
  styleField(pTa);
  lv_obj_t* kp = lv_btnmatrix_create(scrPin);
  lv_btnmatrix_set_map(kp, PIN_MAP);
  lv_obj_set_pos(kp, 4, 48);
  lv_obj_set_size(kp, 312, 188);
  lv_obj_set_style_bg_opa(kp, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(kp, 0, 0);
  lv_obj_set_style_pad_all(kp, 4, 0);
  lv_obj_set_style_pad_gap(kp, 6, 0);
  lv_obj_set_style_text_font(kp, &lv_font_montserrat_20, 0);
  lv_obj_set_style_bg_color(kp, C_CARD, LV_PART_ITEMS);
  lv_obj_set_style_text_color(kp, C_TEXT, LV_PART_ITEMS);
  lv_obj_set_style_radius(kp, 12, LV_PART_ITEMS);
  lv_obj_set_style_shadow_width(kp, 0, LV_PART_ITEMS);
  lv_obj_set_style_bg_color(kp, C_CARD2, LV_PART_ITEMS | LV_STATE_PRESSED);
  lv_obj_add_event_cb(kp, [](lv_event_t* e) {
    lv_obj_t* m = lv_event_get_target(e);
    const char* k = lv_btnmatrix_get_btn_text(m, lv_btnmatrix_get_selected_btn(m));
    if (!k) return;
    if (!strcmp(k, LV_SYMBOL_CLOSE)) { pinNext = nullptr; go(pinBack ? pinBack : scrHome); return; }
    if (!strcmp(k, LV_SYMBOL_OK)) {
      String pin;
      { Lock l; pin = cfg.pin; }
      if (pin == lv_textarea_get_text(pTa)) {
        void (*fn)() = pinNext;
        pinNext = nullptr;
        go(pinBack ? pinBack : scrHome);
        if (fn) fn();
      } else {
        lv_textarea_set_text(pTa, "");
        lv_label_set_text(pMsg, "Wrong PIN");
      }
      return;
    }
    lv_textarea_add_text(pTa, k);
  }, LV_EVENT_VALUE_CHANGED, NULL);
}

// ---------------- SAVER ----------------
static void buildSaver() {
  scrSaver = mkScreen(lv_color_black());
  svTime = mkLabel(scrSaver, &lv_font_montserrat_48, C_TEXT, "");
  lv_obj_align(svTime, LV_ALIGN_CENTER, 0, -30);
  svDate = mkLabel(scrSaver, &lv_font_montserrat_20, C_MUTED, "");
  lv_obj_align(svDate, LV_ALIGN_CENTER, 0, 16);
  svNext = mkLabel(scrSaver, &lv_font_montserrat_16, C_BRAND, "");
  lv_obj_align(svNext, LV_ALIGN_CENTER, 0, 56);
  svWarn = mkLabel(scrSaver, &lv_font_montserrat_14, C_AMBER, "");
  lv_obj_align(svWarn, LV_ALIGN_BOTTOM_MID, 0, -12);
  lv_obj_add_flag(scrSaver, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(scrSaver, [](lv_event_t*) { uiWake(); }, LV_EVENT_CLICKED, NULL);
}

static void refreshSaver() {
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  char b[32];
  if (g_timeOk) {
    strftime(b, sizeof(b), "%H:%M", &lt);
    setText(svTime, b);
    strftime(b, sizeof(b), "%A %d %B", &lt);
    setText(svDate, b);
  } else {
    setText(svTime, "--:--");
    setText(svDate, "");
  }
  Lock l;
  uint8_t nt, ns;
  uint32_t when;
  if (nextDose(nt, ns, when)) setText(svNext, String("Next dose ") + hhmm(when) + "  -  " + cfg.tray[nt].name);
  else setText(svNext, "");
  int dl = daysLeft(1);
  if (ps.stock[1] <= 0) setText(svWarn, LV_SYMBOL_WARNING "  The tray is empty - please refill");
  else if (dl >= 0 && dl <= cfg.lowStockDays) setText(svWarn, String(LV_SYMBOL_WARNING "  Refill soon: ") + ps.stock[1] + " doses left");
  else setText(svWarn, "");
  lv_obj_align(svWarn, LV_ALIGN_BOTTOM_MID, 0, -12);
  lv_obj_align(svTime, LV_ALIGN_CENTER, 0, -30);
  lv_obj_align(svDate, LV_ALIGN_CENTER, 0, 16);
  lv_obj_align(svNext, LV_ALIGN_CENTER, 0, 56);
}

// ---------------- toast ----------------
static void buildToast() {
  toast = mkPanel(lv_layer_top(), 12, 192, 296, 40, C_CARD2, 20);
  lv_obj_set_style_border_color(toast, C_LINE, 0);
  lv_obj_set_style_border_width(toast, 1, 0);
  toastLbl = mkLabel(toast, &lv_font_montserrat_14, C_TEXT, "");
  lv_label_set_long_mode(toastLbl, LV_LABEL_LONG_DOT);
  lv_obj_set_width(toastLbl, 276);
  lv_obj_set_style_text_align(toastLbl, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_center(toastLbl);
  lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(toast, LV_OBJ_FLAG_CLICKABLE);
}

void uiToast(const String& msg) {
  if (!toastQ) return;
  char* s = strdup(msg.c_str());
  if (xQueueSend(toastQ, &s, 0) != pdTRUE) free(s);
}

// ---------------- screenshot & preview over USB serial ----------------
// Send 'P' + a key to show a screen with sample data for 30 s (h home, a alarm, b dispensing, r ready, t tray,
// s settings, w wifi, i info, p pin, v clock, o SOS, m message, f refill), then 'S': the whole screen is redrawn
// and every strip LVGL sends to the panel is copied out (see shotArea), framed by "SCR2 320 240\n" ... "Z\nEND\n".
static void dumpScreen() {
  Serial.print("\nSCR2 320 240\n");
  shotActive = true;
  lv_obj_invalidate(lv_scr_act());
  lv_refr_now(NULL);
  shotActive = false;
  Serial.print("Z\nEND\n");
  Serial.flush();
}

static void previewKey(char k) {
  lv_obj_t* s = nullptr;
  switch (k) {
    case 'h': s = scrHome; break;
    case 'a': s = scrAlert; break;
    case 'b': s = scrBusy; break;
    case 'r': s = scrReady; break;
    case 't': s = scrTray; break;
    case 's': s = scrSettings; break;
    case 'w': s = scrWifi; break;
    case 'i': s = scrInfo; break;
    case 'p': s = scrPin; break;
    case 'v': s = scrSaver; break;
    case 'o': s = scrSos; break;
    case 'm': s = scrMsg; break;
    case 'y': s = scrParty; break;
    case 'f': refStep = 3; refillShow(); s = scrRefill; break;
    case 'x': previewUntil = 0; go(scrHome); return;
    default: return;
  }
  previewUntil = millis() + 30000;
  lastActivity = millis();
  if (s == current) refreshAll();
  else go(s);
}

// Also over USB (tools/settime.py): "T<epoch>\n" sets the clock, "J{json}\n" applies settings such as the time zone.
static void serialTick() {
  static bool armed = false;
  static char mode = 0;
  static String line;
  while (Serial.available()) {
    int c = Serial.read();
    if (mode) {
      if (c == '\n' || c == '\r') {
        if (mode == 'T') postCmd(C_SETTIME, SRC_DEVICE, (int32_t)line.toInt(), 1);
        else if (mode == 'M') {  // M1 alarm, M2 pills ready, M-3 voice /01/003.mp3, M100 party, M101 shabad, M0 stop music
          int n = line.toInt();
          if (n == 200) postCmd(C_ALIGN, SRC_DEVICE, 1);  // M200: one servo push (test)
          else if (n == 0 || (n >= 100 && n < 200)) postCmd(C_PARTY, SRC_DEVICE, n >= 100 ? n - 99 : 0);
          else postCmd(C_SOUND, SRC_DEVICE, n > 0 ? n : 0, n < 0 ? -n : 0);
          Serial.printf("\nsound %d volume %u (now %u) mute %d\n", n, cfg.volume, effectiveVolume(), cfg.mute);
        }
        else postCmd(C_CONFIG, SRC_DEVICE, 0, 0, line.c_str());
        Serial.printf("\nOK %c\n", mode);
        mode = 0;
        line = "";
      } else if (line.length() < 400) line += (char)c;
      continue;
    }
    if (c == 'S') dumpScreen();
    else if (c == 'P') armed = true;
    else if (armed) { armed = false; previewKey((char)c); }
    else if (c == 'D') netDiag();
    else if (c == 'T' || c == 'J' || c == 'M') { mode = (char)c; line = ""; }
  }
}

// ---------------- navigation / power ----------------
void uiStart() { go(scrHome); }
void uiWake() { wakeReq = true; }
bool uiAwake() { return awake; }

static void refreshAll() {
  if (current == scrHome) refreshHome();
  else if (current == scrAlert) refreshAlert();
  else if (current == scrReady) refreshReady();
  else if (current == scrTray) refreshTray();
  else if (current == scrInfo) refreshInfo();
  else if (current == scrSaver) refreshSaver();
  else if (current == scrWifi) refreshWifi();
  else if (current == scrSos) refreshSos();
  else if (current == scrMsg) refreshMsg();
  else if (current == scrBusy) {
    Lock l;
    int p = -1;
    for (int t = 1; t <= MAX_TRAYS; t++) if (act[t].st == DS_DISPENSING) { p = cfg.tray[t].person; break; }
    if (p < 0 && previewing()) p = cfg.tray[1].person;
    setText(bWho, p >= 0 ? cfg.people[p] + "  -  " + cfg.tray[1].name : String("Please wait"));
    lv_obj_align(bWho, LV_ALIGN_CENTER, 0, 68);
  }
}

static void go(lv_obj_t* s) {
  if (s == current) return;
  if (s == scrSettings) refreshSettingsControls();
  current = s;
  refreshAll();
  lv_scr_load(s);
  lastActivity = millis();
}

void uiInit() {
  toastQ = xQueueCreate(6, sizeof(char*));
  lv_init();
  touchSpi.begin(PIN_TOUCH_CLK, PIN_TOUCH_MISO, PIN_TOUCH_MOSI, PIN_TOUCH_CS);
  ts.begin(touchSpi);
  ts.setRotation(3);
  tft.begin();
  tft.setRotation(3);
  applyDisplaySettings();

  lv_disp_draw_buf_init(&drawBuf, buf, NULL, sizeof(buf) / sizeof(buf[0]));
  static lv_disp_drv_t dd;
  lv_disp_drv_init(&dd);
  dd.hor_res = 320;
  dd.ver_res = 240;
  dd.flush_cb = flushCb;
  dd.draw_buf = &drawBuf;
  lv_disp_t* disp = lv_disp_drv_register(&dd);
  static lv_indev_drv_t id;
  lv_indev_drv_init(&id);
  id.type = LV_INDEV_TYPE_POINTER;
  id.read_cb = touchCb;
  lv_indev_drv_register(&id);
  lv_theme_t* th = lv_theme_default_init(disp, C_PRI, C_AMBER, true, &lv_font_montserrat_14);
  lv_disp_set_theme(disp, th);

  buildBoot();
  buildHome();
  buildAlert();
  buildBusy();
  buildReady();
  buildTray();
  buildSettings();
  buildWifi();
  buildInfo();
  buildPin();
  buildSaver();
  buildSos();
  buildMsg();
  buildParty();
  buildRefill();
  buildToast();
  current = scrBoot;
  lv_scr_load(scrBoot);
  lastActivity = millis();
}

void uiTick() {
  lv_timer_handler();
  serialTick();

  char* msg;
  if (xQueueReceive(toastQ, &msg, 0) == pdTRUE) {
    lv_label_set_text(toastLbl, msg);
    free(msg);
    lv_obj_clear_flag(toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(toast);
    toastUntil = millis() + 3500;
  }
  if (toastUntil && millis() > toastUntil) { toastUntil = 0; lv_obj_add_flag(toast, LV_OBJ_FLAG_HIDDEN); }

  static uint32_t lastCheck = 0;
  if (millis() - lastCheck < 150) return;
  lastCheck = millis();
  if (current == scrBoot) return;

  AppMode m;
  uint8_t bright;
  bool clockSaver;
  uint32_t timeout;
  { Lock l; m = appMode(); bright = cfg.brightness; clockSaver = cfg.clockSaver; timeout = cfg.screenTimeout * 1000UL; }

  if (wakeReq) {
    wakeReq = false;
    lastActivity = millis();
    if (current == scrSaver) go(scrHome);
  }

  bool sos, fmsg, party;
  { Lock l; sos = g_sos.on; fmsg = g_msg.unread; party = g_party; }
  bool demo = previewing();
  if (previewUntil && !demo) { previewUntil = 0; lastActivity = millis(); }

  // screens forced by the dose state, an SOS or a family message
  lv_obj_t* want = nullptr;
  if (m == M_DISPENSING) want = scrBusy;
  else if (m == M_READY) want = scrReady;
  else if (sos && current != scrPin) want = scrSos;
  else if (m == M_ALERT && current != scrPin) want = scrAlert;
  else if (fmsg && m == M_IDLE && (current == scrHome || current == scrSaver || current == scrMsg)) want = scrMsg;
  else if (party && m == M_IDLE && current != scrPin) want = scrParty;
  if (demo && !want) {
    // preview: leave the chosen screen alone
  } else {
    if (want && current != want && !(current == scrPin && (want == scrAlert || want == scrSos))) { go(want); lastActivity = millis(); }
    if (!want && (current == scrAlert || current == scrBusy || current == scrReady || current == scrSos || current == scrMsg || current == scrParty)) go(scrHome);
  }

  // screen saver / backlight
  bool forced = m == M_ALERT || m == M_DISPENSING || m == M_READY || sos || (fmsg && current == scrMsg) || party || demo;
  if (current == scrParty) refreshParty();
  if (forced) lastActivity = millis();
  bool idle = millis() - lastActivity > timeout;
  if (idle && current != scrSaver && current != scrPin) { go(scrSaver); lastActivity = millis() - timeout - 1; }
  if (current == scrSaver) {
    awake = false;
    backlight(clockSaver && g_timeOk ? max<int>(5, bright / 4) : 0);
  } else {
    awake = true;
    if (current != scrSettings) backlight(bright);
  }

  static uint32_t lastRefresh = 0;
  if (millis() - lastRefresh > 1000 || (current == scrWifi && wScanning)) {
    lastRefresh = millis();
    refreshAll();
  }
}
