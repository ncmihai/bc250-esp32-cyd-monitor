/*
 * BC-250 live monitor for the ESP32 "Cheap Yellow Display" (ESP32-2432S028R).
 *
 * Polls the BC250's stats service over WiFi and shows a touch dashboard:
 *   Home (tiles) -> tap a tile for details; swipe left/right for Thermals, AI and Settings.
 *   The RGB LED shows the hottest temperature; the screen turns red when critical.
 *
 * Arduino IDE: board "ESP32 Dev Module", Upload Speed 115200, library "LovyanGFX".
 * Edit secrets.h with your WiFi and config.h for the BC250's IP and thresholds.
 * Files: panel.h (display+touch) theme.h (drawing) data.h (network) ui.h (pages).
 */
#include <WiFi.h>
#if !__has_include("secrets.h")
#error "Copy secrets.example.h to secrets.h and put your WiFi name and password in it."
#endif
#include "secrets.h"
#include "ui.h"

// ---------------------------------------------------------------- RGB LED (active low on the CYD)
static const int LED_R = 4, LED_G = 16, LED_B = 17;

static void ledWrite(int r, int g, int b) {
  float k = gLedPct / 100.0f;
  ledcWrite(LED_R, 255 - (int)(r * k));
  ledcWrite(LED_G, 255 - (int)(g * k));
  ledcWrite(LED_B, 255 - (int)(b * k));
}

static void ledInit() {
  ledcAttach(LED_R, 5000, 8);
  ledcAttach(LED_G, 5000, 8);
  ledcAttach(LED_B, 5000, 8);
  ledWrite(0, 0, 0);
}

// green < 60, yellow < 70, red < 80, red blinking < 85, fast red blinking when critical, blue = no link
static void ledUpdate(uint32_t now) {
  if (!gOnline) { ledWrite(0, 0, (int)(255 * (0.55f + 0.45f * sinf(now / 1000.0f * 2.1f)))); return; }   // slow blue pulse
  switch (gLevel) {
    case 0:  ledWrite(0, 255, 0); break;
    case 1:  ledWrite(255, 110, 0); break;
    case 2:  ledWrite(255, 0, 0); break;
    case 3:  ((now / 500) & 1) ? ledWrite(255, 0, 0) : ledWrite(0, 0, 0); break;
    default: ((now / 150) & 1) ? ledWrite(255, 0, 0) : ledWrite(0, 0, 0); break;
  }
}

// ---------------------------------------------------------------- backlight
static bool     gDimmed = false;
static uint32_t gLastTouch = 0;

static void applyBrightness(bool dimmed) {
  int v = gBright * 255 / 100;
  if (dimmed) v = max(6, v / 8);
  tft.setBrightness(v);
}

static void applyBrightnessPct(int pct) { tft.setBrightness(pct * 255 / 100); }
static void startPreview() { gAwayPreviewUntil = millis() + 25000; }

// ---------------------------------------------------------------- touch calibration
static void runCalibration() {
  uint16_t p[8];
  tft.fillScreen(BG);
  tft.setFont(F_TEXT.f);
  tft.setTextColor(TEXT);
  tft.setTextDatum(lgfx::textdatum_t::middle_center);
  tft.drawString("Tap the arrow at each corner", W / 2, H / 2);
  tft.calibrateTouch(p, 0xFFFFFFu, 0x000000u, 24);
  tft.setTouchCalibrate(p);
  prefs.begin("bc250", false);
  prefs.putBytes("cal", p, sizeof p);
  prefs.end();
  Serial.println("touch calibrated and saved");
}

static void touchSetup() {
  uint16_t p[8];
  prefs.begin("bc250", true);
  size_t got = prefs.getBytes("cal", p, sizeof p);
  prefs.end();
  bool force = digitalRead(0) == LOW;           // hold BOOT while resetting to redo the calibration
  if (got == sizeof p && !force) { tft.setTouchCalibrate(p); return; }

  tft.fillScreen(BG);
  tft.setTextDatum(lgfx::textdatum_t::middle_center);
  tft.setFont(F_MID.f);
  tft.setTextColor(TEXT);
  tft.drawString("Touch setup", W / 2, 90);
  tft.setFont(F_TEXT.f);
  tft.setTextColor(DIM);
  tft.drawString("Tap the screen to calibrate", W / 2, 125);
  tft.drawString("(skipping in 10 s)", W / 2, 148);
  uint32_t t0 = millis();
  bool tapped = force;
  while (!tapped && millis() - t0 < 10000) {
    int32_t x, y;
    if (tft.getTouchRaw(&x, &y)) tapped = true;
    delay(30);
  }
  if (tapped) { delay(500); runCalibration(); }
}

// ---------------------------------------------------------------- splash (until the first stats arrive)
static void splash(const char *l1, const char *l2, uint16_t color, const char *l3 = "") {
  static char last[160];
  char key[160];
  snprintf(key, sizeof key, "%s|%s|%s|%u", l1, l2, l3, color);
  if (!strcmp(key, last)) return;
  strlcpy(last, key, sizeof last);
  tft.fillScreen(BG);
  tft.setTextDatum(lgfx::textdatum_t::middle_center);
  tft.setFont(F_MID.f);
  tft.setTextColor(color);
  tft.drawString(l1, W / 2, 95);
  tft.setFont(F_TEXT.f);
  tft.setTextColor(DIM);
  tft.drawString(l2, W / 2, 130);
  tft.drawString(l3, W / 2, 152);
}

// ---------------------------------------------------------------- touch gestures
static void onGesture(int sx, int sy, int lx, int ly, uint32_t now) {
  int dx = lx - sx, dy = ly - sy;
  if (gAway) {                                                      // screen saver: tap = next animation
    if (now < gAwayPreviewUntil && abs(dx) > 50) gAwayPreviewUntil = 0;   // a swipe ends the preview
    else if (awayAnim < 0) gAwayWakeUntil = now + 20000;                  // "screen off": wake it for a while
    else if (abs(dx) < 28 && abs(dy) < 28) awayNext(now);
    return;
  }
  if (gDimmed) { gDimmed = false; applyBrightness(false); gLastTouch = now; return; }   // first touch only wakes
  if (abs(dx) > 50 && abs(dx) > 2 * abs(dy)) onSwipe(dx < 0 ? 1 : -1);
  else if (abs(dx) < 28 && abs(dy) < 28)    onTap(sx, sy);
}

static void pollTouch(uint32_t now) {
  static bool down = false;
  static int sx, sy, lx, ly, samples;
  static uint32_t lastSeen;
  int32_t x, y;
  if (tft.getTouch(&x, &y)) {
    gLastTouch = now;
    lastSeen = now;
    samples++;
    if (!down) { down = true; samples = 1; sx = lx = x; sy = ly = y; }
    if (samples == 2) { sx = x; sy = y; }       // the very first sample of a press is the least accurate
    lx = x; ly = y;
  } else if (down && now - lastSeen > 60) {
    down = false;
    if (samples >= 2) onGesture(sx, sy, lx, ly, now);
  }
}

// ---------------------------------------------------------------- setup / loop
void setup() {
  Serial.begin(115200);
  pinMode(0, INPUT_PULLUP);
  loadSettings();
  applyTheme(gTheme);
  applyStyle(gStyle);

  tft.init();
  tft.setRotation(SCREEN_ROTATION);
  applyBrightness(false);
  tft.fillScreen(BG);
  gSpr.setColorDepth(16);
  gSpr.createSprite(312, 62);
  sSpr.setColorDepth(16);
  sSpr.createSprite(84, 14);
  Serial.printf("display %dx%d, free heap %u\n", (int)tft.width(), (int)tft.height(), (unsigned)ESP.getFreeHeap());

  ledInit();
  ledWrite(255, 0, 0); delay(250);              // power-on LED test: red, green, blue
  ledWrite(0, 255, 0); delay(250);
  ledWrite(0, 0, 255); delay(250);
  ledWrite(0, 0, 0);

  WiFi.mode(WIFI_STA);
  WiFi.setHostname("bc250-display");
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  touchSetup();
  netStart();
  gLastTouch = millis();
}

// ---------------------------------------------------------------- screenshot (tools/screenshot.py)
// Streams the current picture over USB: "SHOT w h", w*h RGB565 pixels (little endian), "END". Takes about 13 s.
static void screenshot() {
  const int w = tft.width(), h = tft.height();
  static lgfx::rgb565_t row[320];
  esp_log_level_set("*", ESP_LOG_NONE);          // system / WiFi log lines would land in the middle of the picture
  Serial.printf("\nSHOT %d %d\n", w, h);
  for (int y = 0; y < h; y++) {
    tft.readRect(0, y, w, 1, row);
    Serial.write((const uint8_t *)row, w * 2);
  }
  Serial.print("\nEND\n");
  Serial.flush();
  esp_log_level_set("*", ESP_LOG_WARN);
}

static void logStatus(uint32_t now) {
  static uint32_t lastLog = 0;
  if (now - lastLog <= 10000) return;
  lastLog = now;
  Serial.printf("ok page=%d ip=%s rssi=%d heap=%u hot=%s %.0fC level=%d online=%d updates=%u away=%d anim=%d frames=%u\n", (int)gPage,
                WiFi.localIP().toString().c_str(), WiFi.RSSI(), (unsigned)ESP.getFreeHeap(), gHot, gHotT, gLevel, (int)gOnline,
                (unsigned)gUpdates, (int)gAway, awayAnim, (unsigned)awayFrames);
}

static bool gShown = false;
static int8_t awayShown = -1;     // -1 unknown, 0 screen off, 1 animating

void loop() {
  static bool lastOnline = false;
  static uint32_t lastOnlineMs = 0;
  static uint32_t lastWifiTry = 0;
  uint32_t now = millis();

  bool changed = syncData();
  pollTouch(now);
  if (Serial.available()) {                      // debug: type 0-6 in the Serial Monitor to jump to a page, t to cycle themes
    char c = Serial.read();
    if (c >= '0' && c <= '6') { gMetric = 0; enterPage((Page)(c - '0')); }
    if (c == 's') screenshot();
    if (c == 'h') { gDebugHost = !gDebugHost; enterPage(gPage); }
    if (c == 'a') startPreview();
    if (c == 'o') { gDebugOffline = !gDebugOffline; Serial.printf("debug offline %d\n", (int)gDebugOffline); }
    if (c == 'n' && gAway) awayNext(millis());
    if (c == 't') { gTheme = (gTheme + 1) % N_THEMES; applyTheme(gTheme); enterPage(gPage); Serial.printf("theme %d %s\n", gTheme, THEMES[gTheme].name); }
  }

  if (WiFi.status() != WL_CONNECTED && now - lastWifiTry > 10000) {
    lastWifiTry = now;
    WiFi.disconnect(); WiFi.begin(WIFI_SSID, WIFI_PASS);
  }

  if (gOnline) lastOnlineMs = now;
  bool preview = now < gAwayPreviewUntil;
  bool away = preview || (!gOnline && now - lastOnlineMs > AWAY_AFTER_MS);
  if (away) {                                    // BC250 off / unreachable (or a Settings preview): screen saver
    bool screenOff = gAwayMode == AW_OFF && !preview && now >= gAwayWakeUntil;
    int want = screenOff ? 0 : 1;
    if (!gAway || want != awayShown) {
      awayShown = want;
      gDimmed = false;
      if (want) { awayEnter(now); applyBrightnessPct(max(15, gBright / 2)); }
      else      { awayStop(); gAway = true; tft.fillScreen(BG); tft.setBrightness(0); }
    }
    awayStep(now, (now - lastOnlineMs) / 1000, preview);
    ledUpdate(now);
    logStatus(now);
    delay(4);
    return;
  }
  if (gAway) {                                   // back online
    awayLeave(); awayShown = -1; gDimmed = false;
    applyBrightness(false);
    gLastTouch = now;
    if (gShown) enterPage(gPage);
    return;
  }

  if (cur.n == 0) {                              // nothing received yet
    if (WiFi.status() != WL_CONNECTED) splash("Connecting to WiFi", WIFI_SSID, TEXT);
    else {
      char l2[48]; snprintf(l2, sizeof l2, "%s:%u", STATS_HOST, STATS_PORT);
      splash("Waiting for the BC250", l2, AMBER, gErr);
    }
    ledUpdate(now);
    delay(30);
    return;
  }

  if (!gShown) { gShown = true; evalAlert(); enterPage(PG_HOME); changed = true; }
  if (gOnline != lastOnline) { lastOnline = gOnline; changed = true; }
  if (changed && evalAlert()) { enterPage(gPage); changed = true; }   // overheat theme flipped: repaint

  if (changed || gRedraw) { gRedraw = false; updatePage(); }

  int8_t frame = gCritical ? (int8_t)((now / 500) & 1) : 0;
  if (frame != gFrameState) { drawAlertFrame(frame); gFrameState = frame; }

  ledUpdate(now);

  uint32_t lim = DIM_MS[gDimIdx];
  bool wantDim = lim && !gCritical && now - gLastTouch > lim;
  if (wantDim != gDimmed) { gDimmed = wantDim; applyBrightness(gDimmed); }

  logStatus(now);
  delay(8);
}
