// =====================================================================
//  PillDispenser Pro - smart medicine dispenser firmware, by Maverick Innovators
//  Hardware: ESP32-2432S028 (CYD) + PCA9685 servo board + WS2812B LEDs + DFPlayer Mini
//
//  Arduino IDE: board "ESP32 Dev Module", esp32 core 2.0.17,
//  Partition Scheme "Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)".
//  Use the libraries folder shipped next to this sketch (lv_conf.h is tuned).
// =====================================================================
#include "app.h"
#include <esp_task_wdt.h>
#include <esp_ota_ops.h>

// Crash protection: count crash resets (panic / watchdog) in RTC memory. A normal power-on starts at 0.
//   3 crashes in a row -> safe mode (no Wi-Fi / web / Telegram, the dispenser itself keeps working)
//   5 crashes in a row -> boot the other firmware slot (the previous version after an OTA update)
// Running 60 s without crashing clears the counter.
#define CRASH_MAGIC 0x5AFE2025
RTC_NOINIT_ATTR static uint32_t rtcMagic, rtcCrashes;

static void crashGuard() {
  esp_reset_reason_t rr = esp_reset_reason();
  bool crash = rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT;
  if (rtcMagic != CRASH_MAGIC || rr == ESP_RST_POWERON) { rtcMagic = CRASH_MAGIC; rtcCrashes = 0; }
  if (crash) rtcCrashes++;
  Serial.printf("reset reason %d, crashes in a row %u\n", (int)rr, rtcCrashes);
  if (rtcCrashes >= 5) {
    const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
    esp_app_desc_t desc;
    if (other && esp_ota_get_partition_description(other, &desc) == ESP_OK && esp_ota_set_boot_partition(other) == ESP_OK) {
      Serial.printf("Rolling back to %s %s\n", desc.project_name, desc.version);
      rtcCrashes = 0;
      ESP.restart();
    }
  }
  g_safeMode = rtcCrashes >= 3;
  if (g_safeMode) Serial.println("SAFE MODE: network disabled");
}

static void bootWait(uint32_t ms, bool (*done)()) {
  uint32_t start = millis();
  while (millis() - start < ms && !(done && done())) {
    esp_task_wdt_reset();
    hwTick();
    lv_timer_handler();
    delay(5);
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\n" FW_NAME " v" FW_VERSION "  -  by " TEAM_NAME " (" TEAM_MEMBERS ")");
  crashGuard();
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdc = {.timeout_ms = 30000, .idle_core_mask = 0, .trigger_panic = true};
  esp_task_wdt_reconfigure(&wdc);
#else
  esp_task_wdt_init(30, true);
#endif
  esp_task_wdt_add(NULL);
  g_lock = xSemaphoreCreateRecursiveMutex();

  storageInit();
  loadConfig();
  loadPersist();
  appInit();

  uiInit();
  uiBoot(10, "Starting hardware...");
  hwInit();
  ledPattern(4, 0);

  uiBoot(25, "Parking servo...");
  for (int t = 1; t <= MAX_TRAYS; t++) servoQueue(t, J_HOME);
  bootWait(8000, [] { return !servoBusy(); });

  uiBoot(55, NET_OFF ? (g_safeMode ? "Safe mode: Wi-Fi off" : "Wi-Fi off") : (cfg.ssid.length() || cfg.ssid2.length()) ? "Connecting Wi-Fi..." : "Starting setup hotspot...");
  netInit();
  if (!NET_OFF && (cfg.ssid.length() || cfg.ssid2.length())) bootWait(12000, [] { return netConnected(); });

  if (netConnected()) {
    uiBoot(80, "Syncing clock...");
    bootWait(6000, [] { return timeValid(); });
  }

  uiBoot(95, "Starting web portal...");
  if (!NET_OFF) {
    webInit();
    tgInit();
  }
  ledPattern(0, 0);
  uiBoot(100, "Ready");
  uiStart();
  if (g_safeMode) uiToast(LV_SYMBOL_WARNING " Safe mode: Wi-Fi off after repeated crashes");
}

void loop() {
  esp_task_wdt_reset();
  if (rtcCrashes && millis() > 60000) rtcCrashes = 0;  // healthy
  hwTick();
  netTick();
  appTick();
  storageTick();
  uiTick();
  delay(2);
}
