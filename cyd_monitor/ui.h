#pragma once
#include <Preferences.h>
#include "theme.h"
#include "away.h"
#include "data.h"

// ================================================================ state
enum Page : uint8_t { PG_HOME, PG_THERM, PG_AI, PG_SETTINGS, PG_CPU, PG_GPU, PG_MEM };
static const Page  TOP_PAGES[] = {PG_HOME, PG_THERM, PG_AI, PG_SETTINGS};   // swipe order
static const int   N_TOP = sizeof(TOP_PAGES) / sizeof(TOP_PAGES[0]);
static const char *PAGE_TITLE[] = {"", "Thermals", "AI", "Settings", "CPU", "GPU", "Memory"};

static Page gPage = PG_HOME;
static int  gMetric = 0;   // selected chip on a detail page
static int  gRange  = 0;   // 0 = 1 min, 1 = 10 min, 2 = 1 h
static const int RANGE_STEP[3] = {1, 4, 24};
static const int RANGE_N[3]    = {60, 150, 150};
static const char *RANGE_LBL[3] = {"1m", "10m", "1h"};

static Store   cur, curX;   // UI-side copies of the latest data
static HistSet curH;
static bool gOnline = false;
static bool gDebugOffline = false;   // debug: Serial 'o' pretends the BC250 is unreachable
static int8_t gFrameState = -1;   // overheat frame: -1 unknown, 0 off, 1 on

// alert state (gCritical lives in theme.h)
static int   gLevel = 0;         // 0 green, 1 yellow, 2 red, 3 red blinking, 4 critical
static char  gHot[8] = "";
static float gHotT = NAN;

// persisted settings
static int gStyle = 0, gBright = 80, gDimIdx = 1, gLedPct = 40, gTheme = 0;
static const uint32_t DIM_MS[4]  = {30000, 60000, 300000, 0};
static const char    *DIM_LBL[4] = {"30 s", "1 min", "5 min", "never"};
static Preferences prefs;

static void loadSettings() {
  prefs.begin("bc250", true);
  gStyle  = prefs.getUChar("style", 0);
  gBright = prefs.getUChar("bright", 80);
  gDimIdx = prefs.getUChar("dim", 1);
  gLedPct = prefs.getUChar("led", 40);
  gTheme  = prefs.getUChar("theme", 0);
  gAwayMode = prefs.getUChar("away", 0);
  prefs.end();
  gStyle = constrain(gStyle, 0, 1); gBright = constrain(gBright, 10, 100);
  gDimIdx = constrain(gDimIdx, 0, 3); gLedPct = constrain(gLedPct, 0, 100);
  gTheme = constrain(gTheme, 0, N_THEMES - 1);
  gAwayMode = constrain(gAwayMode, 0, 5);
}
static void saveSettings() {
  prefs.begin("bc250", false);
  prefs.putUChar("style", gStyle); prefs.putUChar("bright", gBright);
  prefs.putUChar("dim", gDimIdx);  prefs.putUChar("led", gLedPct);
  prefs.putUChar("theme", gTheme);
  prefs.putUChar("away", gAwayMode);
  prefs.end();
}

static float V(const char *k)  { return kvF(cur, k); }
static float VX(const char *k) { return kvF(curX, k); }

static uint32_t seenMain = 0, seenExtra = 0, seenHist = 0;   // versions of what the UI currently holds
static bool gRedraw = true;                                  // set by enterPage(): paint the dynamic parts right away
static uint32_t gUpdates = 0;                                // debug: how many times a page was repainted

static bool syncData() {
  bool ch = false;
  lock();
  if (vMain  != seenMain)  { memcpy(&cur,  &gMain,  sizeof cur);  seenMain  = vMain;  ch = true; }
  if (vExtra != seenExtra) { memcpy(&curX, &gExtra, sizeof curX); seenExtra = vExtra; ch = true; }
  if (vHist  != seenHist)  { memcpy(&curH, &gHist,  sizeof curH); seenHist  = vHist;  ch = true; }
  unlock();
  gOnline = gLinkOk && !gDebugOffline;
  return ch;
}

// ================================================================ alert level
static int levelOf(float t) {
  int l = 0;
  for (int i = 0; i < 4; i++) if (t >= TEMP_LEVELS[i]) l = i + 1;
  return l;
}

// Updates gLevel / gCritical from the hottest of CPU, GPU, VRM and NVMe. Returns true if "critical" flipped.
static bool evalAlert() {
  struct S { const char *n; float t; float off; };
  S s[4] = {{"CPU", V("cpu_temp"), 0}, {"GPU", V("gpu_temp"), 0}, {"VRM", V("board_vrm_temp"), 0},
            {"NVMe", V("board_nvme_temp"), NVME_OFFSET}};
  float hot = -1;
  for (auto &x : s) {
    if (isnan(x.t) || x.t < 1) continue;       // sensors that read 0 are not wired
    float eff = x.t - x.off;
    if (eff > hot) { hot = eff; strlcpy(gHot, x.n, sizeof gHot); gHotT = x.t; }
  }
  if (hot < 0) return false;
  int up = levelOf(hot), down = levelOf(hot + TEMP_HYST);   // hysteresis: must cool a few degrees to drop a level
  if (up > gLevel) gLevel = up; else if (down < gLevel) gLevel = down;
  bool crit = gLevel >= 4;
  if (crit == gCritical) return false;
  gCritical = crit;
  cardC = crit ? CRIT_CARD : CARD;
  hdrC  = crit ? CRIT_HDR : HDR;
  return true;
}

// ================================================================ chrome (header, page dots, alert frame)
static void drawChrome() {
  tft.fillScreen(BG);
  tft.fillRect(0, 0, W, 24, hdrC);
  if (gPage >= PG_CPU) {                       // back chevron
    for (int d = 0; d < 2; d++) {
      tft.drawLine(17 + d, 6, 9 + d, 12, TEXT);
      tft.drawLine(9 + d, 12, 17 + d, 18, TEXT);
    }
  } else {                                     // page dots
    for (int i = 0; i < N_TOP; i++) {
      int x = 160 + (int)((i - (N_TOP - 1) / 2.0f) * 14);
      if (TOP_PAGES[i] == gPage) tft.fillCircle(x, 233, 3, TEXT);
      else                       tft.fillCircle(x, 233, 2, FAINT);
    }
  }
}

static void drawHeaderDyn() {
  char b[40];
  const char *title = PAGE_TITLE[gPage];
  if (gPage == PG_HOME) title = kvS(cur, "host")[0] ? kvS(cur, "host") : "BC250";
  if (gCritical) snprintf(b, sizeof b, "OVERHEAT %s %.0fC", gHot, gHotT);
  else           strlcpy(b, title, sizeof b);
  int tx = gPage >= PG_CPU ? 26 : 10;
  txt(F_LABEL, b, tx, 17, 214 - tx, AL_L, TEXT, hdrC);

  if (!gOnline)        strlcpy(b, "OFFLINE", sizeof b);
  else if (gCritical)  b[0] = 0;                 // no room next to the overheat text
  else if (gPage == PG_SETTINGS) snprintf(b, sizeof b, "WiFi %d dBm", WiFi.RSSI());
  else                 uptimeStr(b, sizeof b, V("uptime_s"));
  txt(F_SMALL, b, 216, 16, 78, AL_R, !gOnline ? (gCritical ? TEXT : RED) : DIM, hdrC);

  uint16_t dot = !gOnline ? ACC[A_CPU] : gLevel == 0 ? GREEN : gLevel == 1 ? AMBER : RED;
  tft.fillCircle(308, 12, 4, gCritical ? TEXT : dot);
}

// Flashing frame around the screen while critical (not over the header).
static void drawAlertFrame(bool on) {
  uint16_t c = on ? RED : BG;
  tft.fillRect(0, 24, 2, H - 24, c);
  tft.fillRect(W - 2, 24, 2, H - 24, c);
  tft.fillRect(0, H - 2, W, 2, c);
}

// ================================================================ HOME
static const int TW = 100, TH = 96;
static const int TILE_X[3] = {4, 110, 216};
static const int TILE_Y[2] = {28, 128};
static const char *TILE_NAME[5] = {"CPU", "GPU", "POWER", "RAM", "VRAM"};
static const Acc   TILE_ACC[5]  = {A_CPU, A_GPU, A_POWER, A_RAM, A_VRAM};

static float pwrRing[60];
static int   pwrN = 0;
static uint32_t pwrVer = 0;
static char  lastKind[8] = "";

static void cacheClearRect(int x, int y, int w, int h) {
  for (auto &c : tcache) {
    int cx = (int)(c.key >> 16) - 1, cy = (int)(c.key & 0xFFFF) - 1;
    if (c.key && cx >= x && cx < x + w && cy >= y && cy < y + h) memset(&c, 0, sizeof c);
  }
  for (auto &c : bcache) {
    int cx = (int)(c.key >> 16) - 1, cy = (int)(c.key & 0xFFFF) - 1;
    if (c.key && cx >= x && cx < x + w && cy >= y && cy < y + h) memset(&c, 0, sizeof c);
  }
}

static void tileFrame(int i) {
  int x = TILE_X[i % 3], y = TILE_Y[i / 3];
  tft.fillRoundRect(x, y, TW, TH, 8, cardC);
  if (gCritical) tft.drawRoundRect(x, y, TW, TH, 8, RED);
}

// label region is kept narrow on tiles that show a percentage on the right of the same row
static void tileLabel(int i, const char *s, uint16_t col, int w) {
  txt(F_LABEL, s, TILE_X[i % 3] + 9, TILE_Y[i / 3] + 17, w, AL_L, col, cardC);
}

static void tilePct(int i, float pct, int x0, int w) {
  char b[12];
  txt(F_TEXT, num(b, sizeof b, pct, "%.0f%%"), TILE_X[i % 3] + x0, TILE_Y[i / 3] + 17, w, AL_R, TEXT, cardC);
}

static void drawHomeStatic() {
  for (int i = 0; i < 6; i++) tileFrame(i);
  for (int i = 0; i < 5; i++) tileLabel(i, TILE_NAME[i], ACC[TILE_ACC[i]], i == 2 ? 84 : 44);
}

static void updateHome() {
  char a[32], b[32], l1[24], l2[24];
  const char *unit;

  // ---- CPU / GPU tiles
  for (int i = 0; i < 2; i++) {
    int x = TILE_X[i], y = TILE_Y[0];
    float t = V(i ? "gpu_temp" : "cpu_temp"), u = V(i ? "gpu_usage" : "cpu_usage");
    tilePct(i, u, 46, 46);
    bigVal(x + 9, y + 50, 84, num(a, sizeof a, t, "%.0f"), "C", tempColor(t), !isnan(t), cardC);
    if (i) num(a, sizeof a, V("gpu_mhz"), "%.0f MHz");
    else   num(a, sizeof a, V("cpu_mhz") / 1000.0f, "%.1f GHz");
    txt(F_SMALL, a, x + 9, y + 74, 84, AL_L, DIM, cardC);
    bar(x + 8, y + 82, 84, 6, u, ACC[TILE_ACC[i]], TRACK);
  }

  // ---- POWER tile
  {
    int x = TILE_X[2], y = TILE_Y[0];
    float w = V("gpu_power_w");
    bigVal(x + 9, y + 50, 84, num(a, sizeof a, w, "%.0f"), "W", TEXT, false, cardC);
    txt(F_SMALL, num(b, sizeof b, V("board_fan_rpm"), "fan %.0f"), x + 9, y + 74, 84, AL_L, DIM, cardC);
    if (seenMain != pwrVer && !isnan(w)) {
      pwrVer = seenMain;
      if (pwrN < 60) pwrRing[pwrN++] = w;
      else { memmove(pwrRing, pwrRing + 1, 59 * sizeof(float)); pwrRing[59] = w; }
    }
    float lo, hi;
    autoRange(pwrRing, pwrN, true, 60, &lo, &hi);
    graphDraw(sSpr, pwrRing, pwrN, 60, lo, hi, ACC[A_POWER], ACCD[A_POWER], cardC, false);
    sSpr.pushSprite(x + 8, y + 78);
  }

  // ---- RAM tile
  {
    int x = TILE_X[0], y = TILE_Y[1];
    float used = V("mem_used"), tot = V("mem_total"), pc = tot > 0 ? 100.0f * used / tot : NAN;
    tilePct(3, pc, 46, 46);
    bytesNum(a, sizeof a, &unit, used);
    bigVal(x + 9, y + 50, 84, a, unit, TEXT, false, cardC);
    gib(b, sizeof b, tot, "of %.1f GB");
    txt(F_SMALL, b, x + 9, y + 74, 84, AL_L, DIM, cardC);
    bar(x + 8, y + 82, 84, 6, pc, ACC[A_RAM], TRACK);
  }

  // ---- VRAM tile (with GTT, where models and games actually land)
  {
    int x = TILE_X[1], y = TILE_Y[1];
    float vu = V("gpu_vram_used"), vt = V("gpu_vram_total"), gu = V("gpu_gtt_used"), gt = V("gpu_gtt_total");
    float vp = vt > 0 ? 100.0f * vu / vt : NAN, gp = gt > 0 ? 100.0f * gu / gt : NAN;
    tilePct(4, vp, 54, 38);
    bytesNum(a, sizeof a, &unit, vu);
    bigVal(x + 9, y + 50, 84, a, unit, TEXT, false, cardC);
    gib(b, sizeof b, gu, "GTT %.1f GB");
    txt(F_SMALL, b, x + 9, y + 74, 84, AL_L, DIM, cardC);
    bar(x + 8, y + 79, 84, 5, vp, ACC[A_VRAM], TRACK);
    bar(x + 8, y + 87, 84, 5, gp, ACC[A_GTT], TRACK);
  }

  // ---- ACTIVITY tile: game / local model / idle
  {
    int x = TILE_X[2], y = TILE_Y[1];
    const char *kind = kvS(cur, "act_kind", "idle");
    const char *name = kvS(cur, "act_name", "");
    if (strcmp(kind, lastKind)) {                 // layout differs per kind: repaint the tile
      strlcpy(lastKind, kind, sizeof lastKind);
      tileFrame(5);
      cacheClearRect(x, y, TW, TH);
    }
    bool game = !strcmp(kind, "game"), llm = !strcmp(kind, "llm");
    tileLabel(5, game ? "GAME" : llm ? "LLM" : "IDLE", ACC[game ? A_GAME : llm ? A_LLM : A_IDLE], 84);
    if (game || llm) {
      wrap2(name, F_TEXT, 82, l1, l2, sizeof l1);
      txt(F_TEXT, l1, x + 9, y + 44, 84, AL_L, TEXT, cardC);
      txt(F_TEXT, l2, x + 9, y + 62, 84, AL_L, TEXT, cardC);
      if (game) num(b, sizeof b, V("gpu_usage"), "GPU %.0f%%");
      else { float sz = V("llm_size"); if (isnan(sz)) strlcpy(b, "loaded", sizeof b); else snprintf(b, sizeof b, "%.1f GB loaded", sz / 1073741824.0f); }
      txt(F_SMALL, b, x + 9, y + 82, 84, AL_L, DIM, cardC);
    } else {
      txt(F_BIG, "Idle", x + 9, y + 50, 84, AL_L, TEXT, cardC);
      txt(F_SMALL, num(b, sizeof b, V("cpu_load1"), "load %.2f"), x + 9, y + 74, 84, AL_L, DIM, cardC);
    }
  }
}

// ================================================================ DETAIL pages (CPU / GPU / MEM)
struct Metric { const char *label, *hkey, *unit; Acc acc; uint8_t kind; float minSpan; };
// kind 0: percent (fixed 0-100), 1: auto range, 2: auto range from zero
static const Metric M_CPU[] = {
  {"TEMP",  "cpu_temp",  "C",   A_CPU, 1, 10},
  {"LOAD",  "cpu_usage", "%",   A_CPU, 0, 100},
  {"CLOCK", "cpu_mhz",   "GHz", A_CPU, 2, 1000},
};
static const Metric M_GPU[] = {
  {"TEMP",  "gpu_temp",  "C",   A_GPU,   1, 10},
  {"LOAD",  "gpu_usage", "%",   A_GPU,   0, 100},
  {"CLOCK", "gpu_mhz",   "MHz", A_GPU,   2, 500},
  {"POWER", "power_w",   "W",   A_POWER, 2, 50},
};
static const Metric M_MEM[] = {
  {"RAM",  "ram_pct",  "", A_RAM,  0, 100},
  {"VRAM", "vram_pct", "", A_VRAM, 0, 100},
  {"GTT",  "gtt_pct",  "", A_GTT,  0, 100},
};

static const Metric *metricsOf(Page p, int *n) {
  if (p == PG_CPU) { *n = 3; return M_CPU; }
  if (p == PG_GPU) { *n = 4; return M_GPU; }
  *n = 3; return M_MEM;
}

static const int CHIP_Y = 28, CHIP_H = 44, RANGE_Y = 76, GRAPH_Y = 98;
static int chipW(int n) { int gap = n == 4 ? 4 : 6; return (312 - (n - 1) * gap) / n; }
static int chipX(int i, int n) { int gap = n == 4 ? 4 : 6; return 4 + i * (chipW(n) + gap); }

static void drawDetailStatic() {
  int n; const Metric *m = metricsOf(gPage, &n);
  gMetric = constrain(gMetric, 0, n - 1);
  for (int i = 0; i < n; i++) {
    int x = chipX(i, n), w = chipW(n);
    bool sel = i == gMetric;
    tft.fillRoundRect(x, CHIP_Y, w, CHIP_H, 7, sel ? CARD2 : cardC);
    tft.drawRoundRect(x, CHIP_Y, w, CHIP_H, 7, sel ? ACC[m[i].acc] : LINE);
    tft.setFont(F_SMALL.f);
    tft.setTextColor(sel ? ACC[m[i].acc] : DIM);
    tft.setTextDatum(lgfx::textdatum_t::baseline_left);
    tft.drawString(m[i].label, x + 8, CHIP_Y + 12);
  }
  for (int i = 0; i < 3; i++) {                  // range buttons
    int x = 196 + i * 40;
    bool sel = i == gRange;
    tft.fillRoundRect(x, RANGE_Y, 36, 18, 5, sel ? ACCD[m[gMetric].acc] : cardC);
    tft.drawRoundRect(x, RANGE_Y, 36, 18, 5, sel ? ACC[m[gMetric].acc] : LINE);
    tft.setFont(F_SMALL.f);
    tft.setTextColor(sel ? TEXT : DIM);
    tft.setTextDatum(lgfx::textdatum_t::baseline_center);
    tft.drawString(RANGE_LBL[i], x + 18, RANGE_Y + 13);
  }
  tft.setFont(F_SMALL.f);
  tft.setTextColor(DIM);
  tft.setTextDatum(lgfx::textdatum_t::baseline_left);
  char t[40]; snprintf(t, sizeof t, "%s history", m[gMetric].label);
  tft.drawString(t, 8, RANGE_Y + 13);
}

static void chipValue(Page p, int idx, char *numBuf, size_t n, const char **unit, bool *deg) {
  *deg = false; *unit = "";
  if (p == PG_CPU) {
    if (idx == 0) { num(numBuf, n, V("cpu_temp"), "%.0f"); *unit = "C"; *deg = true; }
    else if (idx == 1) { num(numBuf, n, V("cpu_usage"), "%.0f"); *unit = "%"; }
    else { num(numBuf, n, V("cpu_mhz") / 1000.0f, "%.1f"); *unit = "GHz"; }
  } else if (p == PG_GPU) {
    if (idx == 0) { num(numBuf, n, V("gpu_temp"), "%.0f"); *unit = "C"; *deg = true; }
    else if (idx == 1) { num(numBuf, n, V("gpu_usage"), "%.0f"); *unit = "%"; }
    else if (idx == 2) { num(numBuf, n, V("gpu_mhz"), "%.0f"); *unit = "MHz"; }
    else { num(numBuf, n, V("gpu_power_w"), "%.0f"); *unit = "W"; }
  } else {
    float v = idx == 0 ? V("mem_used") : idx == 1 ? V("gpu_vram_used") : V("gpu_gtt_used");
    bytesNum(numBuf, n, unit, v);
  }
}

static void vbar(int x, int y, int w, int h, float pct, uint16_t color) {
  int fill = isnan(pct) ? 0 : (int)(constrain(pct, 0.0f, 100.0f) * h / 100.0f + 0.5f);
  uint32_t key = posKey(x, y);
  BCache &c = bcache[(x * 31 + y * 7) % NCACHE];
  if (c.key == key && c.fill == fill && c.color == color) return;
  c.key = key; c.fill = fill; c.color = color;
  tft.fillRoundRect(x, y, w, h, 3, TRACK);
  if (fill > 0) tft.fillRoundRect(x, y + h - max(fill, 4), w, max(fill, 4), 3, color);
}

static void drawDetailGraph(const Metric &m) {
  if (!gSpr.getBuffer()) return;
  const Hist *hh = nullptr;
  for (int i = 0; i < curH.count; i++) if (!strcmp(curH.h[i].key, m.hkey)) hh = &curH.h[i];
  gSpr.setFont(F_SMALL.f);
  gSpr.setTextColor(DIM);
  if (!hh || hh->n == 0) {
    gSpr.fillSprite(cardC);
    gSpr.setTextDatum(lgfx::textdatum_t::middle_center);
    gSpr.drawString("loading...", gSpr.width() / 2, gSpr.height() / 2);
    gSpr.pushSprite(4, GRAPH_Y);
    return;
  }
  float lo, hi;
  if (m.kind == 0) { lo = 0; hi = 100; }
  else autoRange(hh->v, hh->n, m.kind == 2, m.minSpan, &lo, &hi);
  graphDraw(gSpr, hh->v, hh->n, RANGE_N[gRange], lo, hi, ACC[m.acc], ACCD[m.acc], cardC, true);
  char b[16];
  gSpr.setFont(F_SMALL.f);
  gSpr.setTextColor(DIM);
  gSpr.setTextDatum(lgfx::textdatum_t::top_left);
  snprintf(b, sizeof b, "%.0f", hi); gSpr.drawString(b, 5, 3);
  gSpr.setTextDatum(lgfx::textdatum_t::baseline_left);
  snprintf(b, sizeof b, "%.0f", lo); gSpr.drawString(b, 5, gSpr.height() - 4);
  gSpr.pushSprite(4, GRAPH_Y);
}

static void factPair(int row, const char *l1, const char *v1, const char *l2, const char *v2) {
  int base = 184 + row * 18;
  txt(F_SMALL, l1, 8,   base, 70, AL_L, DIM,  BG);
  txt(F_SMALL, v1, 80,  base, 72, AL_R, TEXT, BG);
  txt(F_SMALL, l2, 168, base, 70, AL_L, DIM,  BG);
  txt(F_SMALL, v2, 240, base, 72, AL_R, TEXT, BG);
}

static void updateDetail() {
  int n; const Metric *m = metricsOf(gPage, &n);
  char a[32], b[40];
  const char *unit; bool deg;

  for (int i = 0; i < n; i++) {
    int x = chipX(i, n);
    chipValue(gPage, i, a, sizeof a, &unit, &deg);
    uint16_t col = (gPage != PG_MEM && i == 0) ? tempColor(V(gPage == PG_CPU ? "cpu_temp" : "gpu_temp")) : TEXT;
    bigVal(x + 8, CHIP_Y + 38, chipW(n) - 12, a, unit, col, deg, i == gMetric ? CARD2 : cardC, 3);
  }
  drawDetailGraph(m[gMetric]);

  if (gPage == PG_CPU) {
    int cores = (int)VX("cores");
    if (cores > 0 && cores <= 16) {
      int bw = (312 - (cores - 1) * 4) / cores;
      for (int i = 0; i < cores; i++) {
        char k[12]; snprintf(k, sizeof k, "core%d", i);
        vbar(4 + i * (bw + 4), 166, bw, 36, VX(k), ACC[A_CPU]);
      }
    }
    char line[64];
    snprintf(line, sizeof line, "load %.1f %.1f %.1f  -  %s %s%%", VX("load1"), VX("load5"), VX("load15"),
             kvS(curX, "top1_name", "-"), kvS(curX, "top1_val", "0"));
    fitText(line, F_SMALL, 308);
    txt(F_SMALL, line, 8, 224, 308, AL_L, DIM, BG);
  } else if (gPage == PG_GPU) {
    char v1[24], v2[24], v3[24];
    if (isnan(VX("dpm_cur"))) strlcpy(v3, "--", sizeof v3);
    else snprintf(v3, sizeof v3, "%.0f / %.0f", VX("dpm_cur") + 1, VX("dpm_n"));
    factPair(0, "avg power", num(v1, sizeof v1, VX("power_avg_w"), "%.1f W"), "DPM level", v3);
    factPair(1, "vddgfx", num(v1, sizeof v1, VX("vddgfx_mv"), "%.0f mV"), "vddnb", num(v2, sizeof v2, VX("vddnb_mv"), "%.0f mV"));
    factPair(2, "fan", num(v1, sizeof v1, V("board_fan_rpm"), "%.0f rpm"), "fan pwm", num(v2, sizeof v2, V("board_fan_pwm"), "%.0f %%"));
  } else {
    struct R { const char *name; float used, total; Acc acc; } rows[3] = {
      {"RAM",  V("mem_used"), V("mem_total"), A_RAM},
      {"VRAM", V("gpu_vram_used"), V("gpu_vram_total"), A_VRAM},
      {"GTT",  V("gpu_gtt_used"), V("gpu_gtt_total"), A_GTT}};
    for (int r = 0; r < 3; r++) {
      int base = 182 + r * 16;
      float pc = rows[r].total > 0 ? 100.0f * rows[r].used / rows[r].total : NAN;
      txt(F_SMALL, rows[r].name, 8, base, 42, AL_L, DIM, BG);
      bar(54, base - 8, 180, 9, pc, ACC[rows[r].acc], TRACK);
      char u[16], t[16];
      gib(u, sizeof u, rows[r].used, "%.1f"); gib(t, sizeof t, rows[r].total, "%.1fG");
      snprintf(b, sizeof b, "%s/%s", u, t);
      txt(F_SMALL, b, 240, base, 72, AL_R, TEXT, BG);
    }
    char c[12], sw[12], top[24];
    gib(c, sizeof c, VX("cached"), "%.1fG"); gib(sw, sizeof sw, VX("swap_used"), "%.1fG");
    float tv = VX("top1_val");
    if (isnan(tv)) strlcpy(top, "-", sizeof top); else snprintf(top, sizeof top, "%.1fG", tv / 1073741824.0f);
    char line[72];
    snprintf(line, sizeof line, "cache %s  swap %s  -  %s %s", c, sw, kvS(curX, "top1_name", "-"), top);
    fitText(line, F_SMALL, 308);
    txt(F_SMALL, line, 8, 231, 308, AL_L, DIM, BG);
  }
}

// ================================================================ THERMALS
struct TRow { const char *label, *key, *peak; float off; };
static const TRow T_ROWS[5] = {
  {"CPU",    "cpu_temp",          "peak_cpu_temp",    0},
  {"GPU",    "gpu_temp",          "peak_gpu_temp",    0},
  {"VRM",    "board_vrm_temp",    "peak_vrm_temp",    0},
  {"SYSTEM", "board_system_temp", "peak_system_temp", 0},
  {"NVMe",   "board_nvme_temp",   "peak_nvme_temp",   NVME_OFFSET},
};
static int tRowY(int i) { return 28 + i * 31; }

static void drawThermStatic() {
  for (int i = 0; i < 5; i++) {
    int y = tRowY(i);
    tft.fillRoundRect(4, y, 312, 28, 6, cardC);
    tft.setFont(F_LABEL.f);
    tft.setTextColor(TEXT);
    tft.setTextDatum(lgfx::textdatum_t::baseline_left);
    tft.drawString(T_ROWS[i].label, 12, y + 19);
    tft.drawCircle(249, y + 11, 3, DIM);                 // degree sign + C
    tft.drawCircle(249, y + 11, 2, DIM);
    tft.setFont(F_TEXT.f);
    tft.setTextColor(DIM);
    tft.drawString("C", 255, y + 20);
  }
  int y = tRowY(5);
  tft.fillRoundRect(4, y, 312, 28, 6, cardC);
  tft.setFont(F_LABEL.f);
  tft.setTextColor(TEXT);
  tft.setTextDatum(lgfx::textdatum_t::baseline_left);
  tft.drawString("FAN", 12, y + 19);
  tft.setFont(F_SMALL.f);
  tft.setTextColor(FAINT);
  tft.drawString("pk = peak since the stats service started", 8, 223);
}

static void updateTherm() {
  char a[24];
  for (int i = 0; i < 5; i++) {
    int y = tRowY(i);
    float t = V(T_ROWS[i].key);
    bool wired = !isnan(t) && t > 1;
    float eff = t - T_ROWS[i].off;
    uint16_t col = !wired ? FAINT : eff < TEMP_LEVELS[0] ? GREEN : eff < TEMP_LEVELS[1] ? AMBER : RED;
    bar(90, y + 9, 112, 10, wired ? (t - 20) / 80.0f * 100.0f : 0, col, TRACK);
    txt(F_MID, wired ? num(a, sizeof a, t, "%.0f") : "--", 204, y + 22, 40, AL_R, wired ? TEXT : FAINT, cardC);
    float pk = V(T_ROWS[i].peak);
    txt(F_SMALL, isnan(pk) ? "" : num(a, sizeof a, pk, "pk %.0f"), 268, y + 18, 44, AL_R, DIM, cardC);
  }
  int y = tRowY(5);
  float rpm = V("board_fan_rpm"), pwm = V("board_fan_pwm");
  if (isnan(pwm)) snprintf(a, sizeof a, "%.0f rpm", rpm); else snprintf(a, sizeof a, "%.0f rpm   pwm %.0f%%", rpm, pwm);
  txt(F_TEXT, a, 90, y + 20, 218, AL_L, TEXT, cardC);
}

// ================================================================ AI (ollama)
static void secsStr(char *out, size_t n, float s) {
  if (isnan(s)) { out[0] = 0; return; }
  if (s >= 3600) snprintf(out, n, "%.0fh", s / 3600);
  else if (s >= 60) snprintf(out, n, "%.0fm", s / 60);
  else snprintf(out, n, "%.0fs", s);
}

static void drawAiStatic() {
  tft.fillRoundRect(4, 46, 312, 56, 8, cardC);
  tft.setFont(F_LABEL.f);
  tft.setTextColor(ACC[A_LLM]);
  tft.setTextDatum(lgfx::textdatum_t::baseline_left);
  tft.drawString("LOADED NOW", 8, 41);
  tft.setTextColor(DIM);
  tft.drawString("INSTALLED", 8, 124);
}

static void updateAi() {
  char a[48], b[64], c[16];
  bool up = !strcmp(kvS(curX, "ollama", "?"), "up");
  int loaded = (int)VX("loaded_n");
  if (isnan(VX("loaded_n"))) loaded = 0;

  if (curX.n == 0) {
    txt(F_TEXT, "loading...", 14, 78, 290, AL_L, DIM, cardC);
    txt(F_SMALL, "", 14, 96, 290, AL_L, DIM, cardC);
    return;
  }
  if (!up) {
    txt(F_TEXT, "ollama is not running", 14, 78, 290, AL_L, AMBER, cardC);
    txt(F_SMALL, "", 14, 96, 290, AL_L, DIM, cardC);
  } else if (loaded == 0) {
    txt(F_TEXT, "No model loaded", 14, 76, 290, AL_L, TEXT, cardC);
    txt(F_SMALL, "models load on the first request", 14, 94, 290, AL_L, DIM, cardC);
  } else {
    strlcpy(a, kvS(curX, "m0_name", "?"), sizeof a);
    fitText(a, F_MID, 290);
    txt(F_MID, a, 14, 72, 290, AL_L, TEXT, cardC);
    float sz = VX("m0_size"), vr = VX("m0_vram");
    char gpu[16] = "";
    if (sz > 0 && !isnan(vr)) snprintf(gpu, sizeof gpu, "%.0f%% on GPU", 100.0f * vr / sz);
    secsStr(c, sizeof c, VX("m0_expires"));
    snprintf(b, sizeof b, "%.1f GB  -  %s%s%s%s", isnan(sz) ? 0.0f : sz / 1073741824.0f, gpu, c[0] ? "  -  unloads in " : "", c, loaded > 1 ? "  (+more)" : "");
    fitText(b, F_SMALL, 290);
    txt(F_SMALL, b, 14, 94, 290, AL_L, DIM, cardC);
  }

  int inst = isnan(VX("installed_n")) ? 0 : (int)VX("installed_n");
  for (int i = 0; i < 5; i++) {
    int base = 146 + i * 19;
    if (i < inst) {
      char k[12];
      snprintf(k, sizeof k, "i%d_name", i); strlcpy(a, kvS(curX, k, ""), sizeof a);
      fitText(a, F_TEXT, 230);
      txt(F_TEXT, a, 8, base, 232, AL_L, TEXT, BG);
      snprintf(k, sizeof k, "i%d_size", i); gib(c, sizeof c, VX(k), "%.1f GB");
      txt(F_SMALL, c, 244, base, 68, AL_R, DIM, BG);
    } else {
      txt(F_TEXT, i == 0 && inst == 0 ? "none installed" : "", 8, base, 232, AL_L, DIM, BG);
      txt(F_SMALL, "", 244, base, 68, AL_R, DIM, BG);
    }
  }
}

// ================================================================ SETTINGS
static void applyBrightness(bool dimmed);   // in the main file
static void runCalibration();               // in the main file

static const int SET_N = 6, SET_PITCH = 28;
static int setY(int i) { return 27 + i * SET_PITCH; }     // row tops, each 25 high
static const int RECAL_Y = 196;
static void startPreview();                                 // in the main file

static void drawSettingsStatic() {
  const char *labels[SET_N] = {"Brightness", "Dim screen after", "LED brightness", "Number font", "Theme", "Offline screen"};
  for (int i = 0; i < SET_N; i++) {
    tft.fillRoundRect(4, setY(i), 312, 25, 6, cardC);
    tft.setFont(F_TEXT.f);
    tft.setTextColor(TEXT);
    tft.setTextDatum(lgfx::textdatum_t::baseline_left);
    tft.drawString(labels[i], 12, setY(i) + 18);
  }
  for (int i : {0, 2}) {
    button(166, setY(i) + 2, 40, 21, "-");
    button(270, setY(i) + 2, 40, 21, "+");
  }
  button(8, RECAL_Y, 150, 26, "Recalibrate");
  button(164, RECAL_Y, 148, 26, "Preview away");
}

static void updateSettings() {
  char a[40];
  snprintf(a, sizeof a, "%d%%", gBright);  txt(F_LABEL, a, 210, setY(0) + 18, 56, AL_C, TEXT, cardC);
  txt(F_LABEL, DIM_LBL[gDimIdx], 166, setY(1) + 18, 144, AL_C, ACC[A_VRAM], cardC);
  if (gLedPct == 0) strlcpy(a, "off", sizeof a); else snprintf(a, sizeof a, "%d%%", gLedPct);
  txt(F_LABEL, a, 210, setY(2) + 18, 56, AL_C, TEXT, cardC);
  txt(F_LABEL, gStyle ? "Tech" : "Clean", 166, setY(3) + 18, 144, AL_C, ACC[A_VRAM], cardC);
  txt(F_LABEL, THEMES[gTheme].name, 166, setY(4) + 18, 144, AL_C, ACC[A_VRAM], cardC);
  txt(F_LABEL, AWAY_NAMES[gAwayMode], 166, setY(5) + 18, 144, AL_C, ACC[A_VRAM], cardC);
}

// ================================================================ page dispatch
static void requestFor(Page p) {
  const char *extra = "", *hist = "";
  if (p == PG_CPU) { extra = "cpu"; hist = M_CPU[gMetric].hkey; }
  else if (p == PG_GPU) { extra = "gpu"; hist = M_GPU[gMetric].hkey; }
  else if (p == PG_MEM) { extra = "mem"; hist = M_MEM[gMetric].hkey; }
  else if (p == PG_AI) extra = "ai";
  netRequest(extra, hist, RANGE_STEP[gRange], RANGE_N[gRange]);
}

static void enterPage(Page p) {
  Serial.printf("page -> %d\n", (int)p);
  gPage = p;
  cacheReset();
  lastKind[0] = 0;
  gFrameState = -1;
  drawChrome();
  switch (p) {
    case PG_HOME:     drawHomeStatic(); break;
    case PG_CPU: case PG_GPU: case PG_MEM: drawDetailStatic(); break;
    case PG_THERM:    drawThermStatic(); break;
    case PG_AI:       drawAiStatic(); break;
    case PG_SETTINGS: drawSettingsStatic(); break;
  }
  requestFor(p);
  syncData();
  drawHeaderDyn();
  gRedraw = true;
}

static void updatePage() {
  gUpdates++;
  drawHeaderDyn();
  switch (gPage) {
    case PG_HOME:     updateHome(); break;
    case PG_CPU: case PG_GPU: case PG_MEM: updateDetail(); break;
    case PG_THERM:    updateTherm(); break;
    case PG_AI:       updateAi(); break;
    case PG_SETTINGS: updateSettings(); break;
  }
}

// ================================================================ touch
static void onSwipe(int next) {            // +1 = next page (finger moved left), -1 = previous
  if (gPage >= PG_CPU) { if (next < 0) enterPage(PG_HOME); return; }
  int idx = 0;
  for (int i = 0; i < N_TOP; i++) if (TOP_PAGES[i] == gPage) idx = i;
  int to = idx + next;
  if (to >= 0 && to < N_TOP) enterPage(TOP_PAGES[to]);
}

static void onTap(int x, int y) {
  if (gPage >= PG_CPU) {
    if (y < 28 && x < 110) { enterPage(PG_HOME); return; }
    int n; const Metric *m = metricsOf(gPage, &n); (void)m;
    for (int i = 0; i < n; i++)
      if (inRect(x, y, chipX(i, n) - 2, CHIP_Y - 2, chipW(n) + 4, CHIP_H + 4) && i != gMetric) { gMetric = i; enterPage(gPage); return; }
    for (int i = 0; i < 3; i++)
      if (inRect(x, y, 196 + i * 40 - 2, RANGE_Y - 4, 40, 26) && i != gRange) { gRange = i; enterPage(gPage); return; }
    return;
  }
  switch (gPage) {
    case PG_HOME:
      for (int i = 0; i < 6; i++) {
        if (!inRect(x, y, TILE_X[i % 3] - 3, TILE_Y[i / 3] - 3, TW + 6, TH + 6)) continue;
        gMetric = 0; gRange = 0;
        if (i == 0) enterPage(PG_CPU);
        else if (i == 1) enterPage(PG_GPU);
        else if (i == 2) { gMetric = 3; enterPage(PG_GPU); }
        else if (i == 3) enterPage(PG_MEM);
        else if (i == 4) { gMetric = 1; enterPage(PG_MEM); }
        else enterPage(!strcmp(kvS(cur, "act_kind"), "game") ? PG_GPU : PG_AI);
        return;
      }
      break;
    case PG_SETTINGS: {
      bool changed = true;
      auto row = [&](int i, int x0, int w) { return inRect(x, y, x0, setY(i) - 1, w, SET_PITCH); };
      if (row(0, 160, 56)) gBright = max(10, gBright - 10);
      else if (row(0, 262, 56)) gBright = min(100, gBright + 10);
      else if (row(1, 160, 158)) gDimIdx = (gDimIdx + 1) % 4;
      else if (row(2, 160, 56)) gLedPct = max(0, gLedPct - 10);
      else if (row(2, 262, 56)) gLedPct = min(100, gLedPct + 10);
      else if (row(3, 160, 158)) { gStyle ^= 1; applyStyle(gStyle); saveSettings(); enterPage(PG_SETTINGS); return; }
      else if (row(4, 160, 158)) { gTheme = (gTheme + 1) % N_THEMES; applyTheme(gTheme); saveSettings(); enterPage(PG_SETTINGS); return; }
      else if (row(5, 160, 158)) gAwayMode = (gAwayMode + 1) % 6;
      else if (inRect(x, y, 160, RECAL_Y - 4, 158, 34)) { saveSettings(); startPreview(); return; }
      else if (inRect(x, y, 4, RECAL_Y - 4, 156, 34)) { runCalibration(); enterPage(PG_SETTINGS); return; }
      else changed = false;
      if (changed) { applyBrightness(false); saveSettings(); updateSettings(); }
      break;
    }
    default: break;
  }
}
