// Servos (PCA9685), status LEDs (WS2812B), DFPlayer audio, physical button, backlight
#include "app.h"
#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>
#include <Adafruit_NeoPixel.h>
#include <DFRobotDFPlayerMini.h>

static Adafruit_PWMServoDriver pwm;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
// Core 3.x: the bundled NeoPixel 1.1.2 bit-bangs with interrupts off and trips the watchdog,
// so drive the WS2812B LEDs with the RMT peripheral instead.
static bool rmtOk = false;
static uint32_t rgbColor(uint8_t r, uint8_t g, uint8_t b) { return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b; }
static void ledWrite2(uint32_t a, uint32_t b);
static void ledWrite(uint32_t rgb) { ledWrite2(rgb, rgb); }
static void ledWrite2(uint32_t a, uint32_t) {
  uint32_t rgb = a;
  if (!rmtOk) return;
  uint8_t r = (rgb >> 16) * 50 / 255, g = ((rgb >> 8) & 0xFF) * 50 / 255, b = (rgb & 0xFF) * 50 / 255;  // brightness 50
  uint8_t bytes[3] = {g, r, b};  // GRB order
  rmt_data_t d[NUM_LEDS * 24];
  int i = 0;
  for (int led = 0; led < NUM_LEDS; led++)
    for (int k = 0; k < 3; k++)
      for (int bit = 7; bit >= 0; bit--) {
        bool one = bytes[k] & (1 << bit);
        d[i].level0 = 1; d[i].duration0 = one ? 8 : 4;  // 10 MHz ticks: 0.8 / 0.4 us high
        d[i].level1 = 0; d[i].duration1 = one ? 4 : 8;
        i++;
      }
  rmtWrite(PIN_LED, d, i, RMT_WAIT_FOR_EVER);
}
#else
static Adafruit_NeoPixel pixels(NUM_LEDS, PIN_LED, NEO_GRB + NEO_KHZ800);
static uint32_t rgbColor(uint8_t r, uint8_t g, uint8_t b) { return pixels.Color(r, g, b); }
static void ledWrite(uint32_t rgb) {
  for (int i = 0; i < NUM_LEDS; i++) pixels.setPixelColor(i, rgb);
  pixels.show();
}
static void ledWrite2(uint32_t a, uint32_t b) {
  for (int i = 0; i < NUM_LEDS; i++) pixels.setPixelColor(i, i % 2 ? b : a);
  pixels.show();
}
#endif
static DFRobotDFPlayerMini player;

void (*onServoJobDone)(uint8_t tray, JobKind kind) = nullptr;
void (*onButton)(uint8_t kind) = nullptr;

// ---------------- Servo job queue ----------------
// One "cycle" = push to servoPush, hold pushMs, back to servoRest, hold restMs.
// A dose (one compartment) = cyclesPerDose cycles (2 on the original mechanism).
struct Job { uint8_t tray; JobKind kind; uint16_t cycles; };
static Job jobs[24];
static uint8_t jobHead = 0, jobCount = 0;
static bool busy = false;
static Job cur;
static uint16_t cyclesLeft;
static bool pushing;
static uint32_t phaseEnd;

static bool pcaOk = false;
static uint32_t pcaChecked = 0;

static bool pcaProbe() {
  Wire.beginTransmission(0x40);
  return Wire.endTransmission() == 0;
}

static void pcaSetup() {
  pwm.begin();
  pwm.setPWMFreq(SERVO_FREQ);
}

bool servoBoardOk() { return pcaOk; }

static void pcaTick() {  // base PCB may be plugged in / powered later, or unplugged
  if (busy || millis() - pcaChecked < 3000) return;
  pcaChecked = millis();
  bool ok = pcaProbe();
  if (ok && !pcaOk) pcaSetup();
  if (ok != pcaOk) Serial.println(ok ? "Servo board (PCA9685) found" : "Servo board (PCA9685) lost");
  pcaOk = ok;
}

// Single tray: the same signal goes to every tray header (PCA9685 outputs 0..SERVO_HEADERS-1), so the
// servo works whichever header it is plugged into ("Tray 1" = output 0 ... "Tray 10" = output 9).
static void servoAngle(uint8_t, uint8_t deg) {
  uint16_t p = map(deg, 0, 180, SERVO_PULSE_MIN, SERVO_PULSE_MAX);
  for (uint8_t ch = 0; ch < SERVO_HEADERS; ch++) pwm.setPWM(ch, 0, p);
  Serial.printf("servo (all %u headers) -> %u deg\n", SERVO_HEADERS, deg);
}
static void servoRelease(uint8_t) {
  for (uint8_t ch = 0; ch < SERVO_HEADERS; ch++) pwm.setPWM(ch, 0, 0);
}

bool servoQueue(uint8_t tray, JobKind kind, uint8_t steps) {
  if (tray < 1 || tray > MAX_TRAYS || jobCount >= 24) return false;
  Job j{tray, kind, 0};
  switch (kind) {
    case J_HOME: j.cycles = 0; break;
    case J_ALIGN: j.cycles = 1; break;
    default: j.cycles = (uint16_t)steps * cfg.cyclesPerDose; break;
  }
  jobs[(jobHead + jobCount) % 24] = j;
  jobCount++;
  return true;
}

bool servoBusy() { return busy || jobCount > 0; }

static void servoTick() {
  uint32_t now = millis();
  if (!busy) {
    if (!jobCount) return;
    cur = jobs[jobHead];
    jobHead = (jobHead + 1) % 24;
    jobCount--;
    busy = true;
    cyclesLeft = cur.cycles;
    if (cyclesLeft == 0) {  // homing: just park at rest
      pushing = false;
      servoAngle(cur.tray - 1, cfg.servoRest);
      phaseEnd = now + cfg.restMs;
    } else {
      pushing = true;
      servoAngle(cur.tray - 1, cfg.servoPush);
      phaseEnd = now + cfg.pushMs;
    }
    return;
  }
  if ((int32_t)(now - phaseEnd) < 0) return;
  if (pushing) {
    pushing = false;
    servoAngle(cur.tray - 1, cfg.servoRest);
    phaseEnd = now + cfg.restMs;
    return;
  }
  if (cyclesLeft > 0) cyclesLeft--;
  if (cyclesLeft > 0) {
    pushing = true;
    servoAngle(cur.tray - 1, cfg.servoPush);
    phaseEnd = now + cfg.pushMs;
    return;
  }
  servoRelease(cur.tray - 1);
  busy = false;
  if (onServoJobDone) onServoJobDone(cur.tray, cur.kind);
}

// ---------------- LEDs ----------------
static uint8_t ledMode = 0;
static uint32_t ledColor = 0, lastShown = 0xFFFFFFFF;

static void ledShow(uint32_t rgb) {
  if (rgb == lastShown) return;
  lastShown = rgb;
  ledWrite(rgb);
}

void ledSet(uint32_t rgb) { ledPattern(rgb ? 1 : 0, rgb); }
void ledPattern(uint8_t pattern, uint32_t rgb) { ledMode = pattern; ledColor = rgb; }

static void ledTick() {
  uint32_t t = millis();
  switch (ledMode) {
    case 0: ledShow(0); break;
    case 1: ledShow(ledColor); break;
    case 2: ledShow((t / 500) % 2 ? ledColor : 0); break;
    case 3: ledShow((t / 120) % 2 ? ledColor : 0); break;
    case 5: {  // disco: each LED jumps to a new bright colour every beat, with short strobe flashes
      static uint32_t next = 0;
      static uint8_t step = 0;
      static const uint32_t DISCO[] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFF00FF, 0xFFFF00, 0x00FFFF, 0xFF6000, 0xFFFFFF};
      if ((int32_t)(t - next) < 0) break;
      step++;
      next = t + (step % 8 == 0 ? 60 : 140);
      if (step % 8 == 0) ledWrite2(0, 0);
      else ledWrite2(DISCO[esp_random() % 8], DISCO[esp_random() % 8]);
      lastShown = 0xFFFFFFFF;  // force the next normal pattern to redraw
      break;
    }
    case 6: {  // slow breathing in ledColor (4 s per breath, never fully dark)
      uint32_t ph = t % 4000;
      uint32_t k = 40 + (ph < 2000 ? ph : 4000 - ph) * 215 / 2000;  // 40..255
      ledShow(((((ledColor >> 16) & 0xFF) * k / 255) << 16) | ((((ledColor >> 8) & 0xFF) * k / 255) << 8) | ((ledColor & 0xFF) * k / 255));
      break;
    }
    case 4: {  // colour wheel
      uint8_t p = (t / 8) & 0xFF, s = p % 85 * 3;
      ledShow(p < 85 ? rgbColor(255 - s, s, 0) : p < 170 ? rgbColor(0, 255 - s, s) : rgbColor(s, 0, 255 - s));
      break;
    }
  }
}

// ---------------- Audio ----------------
uint8_t effectiveVolume() {
  if (cfg.quiet && timeValid()) {
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    int h = lt.tm_hour;
    bool in = cfg.quietFrom <= cfg.quietTo ? (h >= cfg.quietFrom && h < cfg.quietTo)
                                           : (h >= cfg.quietFrom || h < cfg.quietTo);
    if (in) return cfg.quietVolume;
  }
  return cfg.volume;
}

static uint32_t lastPlay = 0;
static int8_t sentVolume = -1;
// Clone DFPlayer chips drop a command that follows another too quickly: only send the volume when it changed, then pause.
static void setVolume() {
  uint8_t v = constrain(effectiveVolume(), 0, 30);
  if (v == sentVolume) return;
  player.volume(v);
  sentVolume = v;
  delay(60);
}
void playTrack(uint8_t track) {
  if (cfg.mute) return;
  setVolume();
  player.play(track);
  lastPlay = millis();
  Serial.printf("sound: track %u\n", track);
}
void playVoice(uint8_t file) {
  if (cfg.mute) return;
  setVolume();
  player.playFolder(1, file);
  lastPlay = millis();
  Serial.printf("sound: /01/%03u.mp3\n", file);
}
void audioReset() { player.reset(); }

// Every song in the folder one after another, then from the start again:
// party = SD folder "02" (001.mp3, 002.mp3, ...), shabad = SD folder "03".
void playParty(uint8_t kind) {
  if (cfg.mute) return;
  setVolume();
  uint8_t folder = kind == MUSIC_SHABAD ? 3 : 2;
  player.loopFolder(folder);
  Serial.printf("sound: folder /%02u/ on repeat\n", folder);
}
void stopAudio() {
  player.disableLoop();
  delay(60);
  player.stop();
  delay(60);
}

// ---------------- Backlight ----------------
static uint8_t blNow = 255;
static bool blReady = false;  // PWM channel is set up in hwInit(); the display driver switches the pin on before that
void backlight(uint8_t pct) {
  if (pct > 100) pct = 100;
  if (!blReady || pct == blNow) return;
  blNow = pct;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_BACKLIGHT, pct == 0 ? 0 : map(pct, 1, 100, 3, 255));
#else
  ledcWrite(7, pct == 0 ? 0 : map(pct, 1, 100, 3, 255));
#endif
}

// ---------------- Button ----------------
static bool btnDown = false, longFired = false, sosFired = false, rawLast = HIGH;
static uint32_t rawChange = 0, downAt = 0;

static void buttonTick() {
  bool raw = digitalRead(PIN_BUTTON);
  uint32_t now = millis();
  if (raw != rawLast) { rawLast = raw; rawChange = now; }
  if (now - rawChange < 40) return;
  bool down = raw == LOW;
  if (down && !btnDown) { btnDown = true; longFired = false; sosFired = false; downAt = now; }
  if (down && btnDown && !longFired && now - downAt > 1500) {
    longFired = true;
    if (onButton) onButton(1);
  }
  if (down && btnDown && longFired && !sosFired && now - downAt > 5000) {
    sosFired = true;
    if (onButton) onButton(2);
  }
  if (!down && btnDown) {
    btnDown = false;
    if (!longFired && onButton) onButton(0);
  }
}

// ---------------- Init / tick ----------------
void hwInit() {
  pinMode(PIN_BUTTON, INPUT_PULLUP);
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttachChannel(PIN_BACKLIGHT, 5000, 8, 7);
#else
  ledcSetup(7, 5000, 8);
  ledcAttachPin(PIN_BACKLIGHT, 7);
#endif
  blReady = true;
  backlight(cfg.brightness);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  pcaOk = pcaProbe();
  pcaChecked = millis();
  pcaSetup();
  Serial.println(pcaOk ? "Servo board (PCA9685) found" : "Servo board (PCA9685) NOT found on I2C 0x40 - check base PCB cable/power");

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  rmtOk = rmtInit(PIN_LED, RMT_TX_MODE, RMT_MEM_NUM_BLOCKS_1, 10000000);
#else
  pixels.begin();
  pixels.setBrightness(50);
#endif
  ledShow(0);

  // DFPlayer is wired TX-only, so run it without ACK (otherwise every command blocks 500 ms)
  Serial2.begin(9600, SERIAL_8N1, -1, PIN_DFPLAYER_TX);
  player.begin(Serial2, false, true);
  player.outputDevice(DFPLAYER_DEVICE_SD);  // some clones need the SD card selected explicitly
  delay(200);
  setVolume();
}

void hwTick() {
  pcaTick();
  servoTick();
  ledTick();
  buttonTick();
}
