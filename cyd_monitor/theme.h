#pragma once
#include <math.h>
#include "panel.h"

// ---------------------------------------------------------------- display + sprites
static CYD tft;
static lgfx::LGFX_Sprite gSpr(&tft);  // detail-page history graph, 312 x 72
static lgfx::LGFX_Sprite sSpr(&tft);  // sparkline inside the power tile, 84 x 14

static const int W = 320, H = 240;

// ---------------------------------------------------------------- palette and themes
#define C565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

// Same in every theme: text, and the traffic-light colours used for temperatures.
static const uint16_t TEXT   = C565(236, 240, 248);
static const uint16_t GREEN  = C565(84, 214, 140);
static const uint16_t AMBER  = C565(255, 184, 64);
static const uint16_t RED    = C565(255, 92, 70);
// Overheat theme replaces the chosen one while critical
static const uint16_t CRIT_HDR  = C565(170, 22, 28);
static const uint16_t CRIT_CARD = C565(44, 17, 21);

static bool gCritical = false;

// accent slots used by tiles, graphs and bars
enum Acc : uint8_t { A_CPU, A_GPU, A_POWER, A_RAM, A_VRAM, A_GTT, A_GAME, A_LLM, A_IDLE, A_N };

struct Theme {
  const char *name;
  uint16_t bg, hdr, card, card2, line, track, dim;
  uint16_t acc[A_N];
};
static const int N_THEMES = 5;
static const Theme THEMES[N_THEMES] = {
  {"Default", C565(7, 9, 15), C565(14, 18, 28), C565(20, 25, 36), C565(30, 37, 52), C565(40, 49, 68), C565(36, 44, 62), C565(130, 142, 166),
   {C565(84, 172, 255), C565(255, 96, 72), C565(60, 214, 200), C565(84, 214, 140), C565(255, 184, 64),
    C565(176, 128, 255), C565(246, 108, 196), C565(176, 128, 255), C565(120, 140, 172)}},
  {"Cyan", C565(3, 11, 17), C565(7, 25, 36), C565(10, 29, 41), C565(15, 43, 60), C565(24, 66, 88), C565(17, 47, 64), C565(116, 160, 178),
   {C565(0, 200, 255), C565(90, 222, 255), C565(0, 232, 232), C565(70, 205, 245), C565(140, 238, 255),
    C565(30, 165, 225), C565(180, 245, 255), C565(100, 205, 255), C565(90, 150, 180)}},
  {"Turquoise", C565(3, 14, 12), C565(6, 30, 27), C565(8, 34, 31), C565(12, 50, 45), C565(20, 74, 67), C565(14, 52, 47), C565(112, 168, 158),
   {C565(64, 224, 208), C565(120, 244, 205), C565(30, 205, 180), C565(95, 232, 170), C565(160, 252, 225),
    C565(40, 185, 160), C565(190, 255, 238), C565(100, 222, 200), C565(90, 160, 150)}},
  {"Pink", C565(14, 6, 12), C565(32, 10, 27), C565(37, 14, 33), C565(55, 20, 49), C565(86, 32, 76), C565(58, 24, 52), C565(176, 128, 160),
   {C565(255, 110, 190), C565(255, 156, 214), C565(240, 80, 160), C565(255, 176, 224), C565(255, 125, 165),
    C565(205, 115, 255), C565(255, 205, 238), C565(225, 145, 255), C565(172, 112, 152)}},
  {"Navy", C565(3, 6, 20), C565(8, 16, 50), C565(12, 24, 66), C565(18, 36, 94), C565(34, 58, 132), C565(22, 38, 98), C565(130, 150, 205),
   {C565(110, 170, 255), C565(150, 200, 255), C565(90, 150, 255), C565(130, 190, 255), C565(185, 218, 255),
    C565(100, 132, 255), C565(205, 228, 255), C565(142, 162, 255), C565(112, 132, 192)}},
};

// current theme (filled by applyTheme)
static uint16_t BG, HDR, CARD, CARD2, LINE, TRACK, DIM, FAINT;
static uint16_t ACC[A_N], ACCD[A_N];     // accent and its dark variant (graph fill)
static uint16_t cardC, hdrC;             // card / header colour right now (red while critical)

static uint16_t mix565(uint16_t a, uint16_t b, int t) {   // t/255 of a, the rest of b
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  return (uint16_t)((((ar * t + br * (255 - t)) / 255) << 11) | (((ag * t + bg * (255 - t)) / 255) << 5) | ((ab * t + bb * (255 - t)) / 255));
}

static void applyTheme(int i) {
  const Theme &t = THEMES[constrain(i, 0, N_THEMES - 1)];
  BG = t.bg; HDR = t.hdr; CARD = t.card; CARD2 = t.card2; LINE = t.line; TRACK = t.track; DIM = t.dim;
  FAINT = mix565(t.dim, t.bg, 110);
  for (int k = 0; k < A_N; k++) { ACC[k] = t.acc[k]; ACCD[k] = mix565(t.acc[k], t.bg, 70); }
  cardC = gCritical ? CRIT_CARD : CARD;
  hdrC  = gCritical ? CRIT_HDR : HDR;
}

// Temperature colour, matching the LED: green < 60 <= yellow < 70 <= orange/red
static uint16_t tempColor(float t) {
  if (isnan(t)) return DIM;
  if (t < TEMP_LEVELS[0]) return GREEN;
  if (t < TEMP_LEVELS[1]) return AMBER;
  return RED;
}

// ---------------------------------------------------------------- fonts
// asc/desc are the real ascent/descent in pixels (measured from the font data), so text can be
// placed by its baseline and its background cleared exactly.
struct FontSpec { const lgfx::IFont *f; int8_t asc, desc; };
static FontSpec F_BIG, F_MID, F_LABEL, F_TEXT, F_SMALL;

static void applyStyle(int style) {
  F_LABEL = {&lgfx::fonts::FreeSansBold9pt7b, 12, 5};
  F_TEXT  = {&lgfx::fonts::FreeSans9pt7b, 12, 5};
  F_SMALL = {&lgfx::fonts::DejaVu12, 8, 4};
  if (style == 1) {  // "Tech"
    F_BIG = {&lgfx::fonts::Orbitron_Light_32, 24, 8};
    F_MID = {&lgfx::fonts::Orbitron_Light_24, 17, 6};
  } else {           // "Clean"
    F_BIG = {&lgfx::fonts::FreeSansBold18pt7b, 24, 8};
    F_MID = {&lgfx::fonts::FreeSansBold12pt7b, 16, 6};
  }
}

enum Al : uint8_t { AL_L, AL_C, AL_R };

static int textW(const FontSpec &fs, const char *s) {
  tft.setFont(fs.f);
  return tft.textWidth(s);
}

// ---------------------------------------------------------------- flicker-free text and bars
// Every draw is remembered by position; unchanged text / bars never touch the SPI bus.
struct TCache { uint32_t key; uint16_t fg, bg; char s[30]; };
struct BCache { uint32_t key; int16_t fill; uint16_t color; };
static const int NCACHE = 128;
static TCache tcache[NCACHE];
static BCache bcache[NCACHE];

static void cacheReset() {
  memset(tcache, 0, sizeof(tcache));
  memset(bcache, 0, sizeof(bcache));
}

static inline uint32_t posKey(int x, int y) { return ((uint32_t)(x + 1) << 16) | (uint32_t)(y + 1); }

static void txt(const FontSpec &fs, const char *s, int x, int base, int w, Al al, uint16_t fg, uint16_t bg) {
  uint32_t key = posKey(x, base);
  TCache &c = tcache[(x * 31 + base * 7) % NCACHE];
  if (c.key == key && c.fg == fg && c.bg == bg && !strncmp(c.s, s, sizeof(c.s) - 1)) return;
  c.key = key; c.fg = fg; c.bg = bg;
  strlcpy(c.s, s, sizeof(c.s));
  tft.fillRect(x, base - fs.asc - 1, w, fs.asc + fs.desc + 2, bg);
  tft.setFont(fs.f);
  tft.setTextColor(fg);
  tft.setTextDatum(al == AL_L ? lgfx::textdatum_t::baseline_left
                  : al == AL_R ? lgfx::textdatum_t::baseline_right
                               : lgfx::textdatum_t::baseline_center);
  tft.drawString(s, al == AL_L ? x : al == AL_R ? x + w : x + w / 2, base);
}

// A big number with its unit next to it. Uses the medium font when the big one does not fit, and drops
// the unit if even that does not fit. `below` limits how far under the baseline the background is cleared.
static void bigVal(int x, int base, int w, const char *num, const char *unit, uint16_t col, bool deg, uint16_t bg, int below = -1) {
  char sig[30];
  snprintf(sig, sizeof sig, "%s|%s|%d", num, unit, deg);
  uint32_t key = posKey(x, base);
  TCache &c = tcache[(x * 31 + base * 7) % NCACHE];
  if (c.key == key && c.fg == col && c.bg == bg && !strncmp(c.s, sig, sizeof(c.s) - 1)) return;
  c.key = key; c.fg = col; c.bg = bg;
  strlcpy(c.s, sig, sizeof(c.s));

  int unitW = unit[0] ? textW(F_TEXT, unit) + (deg ? 9 : 0) + 4 : 0;
  const FontSpec *fs = &F_BIG;
  bool showUnit = true;
  if (textW(F_BIG, num) + unitW > w) {
    fs = &F_MID;
    if (textW(F_MID, num) + unitW > w) showUnit = false;
  }
  int under = below >= 0 ? below : F_BIG.desc + 1;
  tft.fillRect(x, base - F_BIG.asc - 1, w, F_BIG.asc + 1 + under, bg);
  tft.setFont(fs->f);
  tft.setTextColor(col);
  tft.setTextDatum(lgfx::textdatum_t::baseline_left);
  tft.drawString(num, x, base);
  if (!showUnit || !unit[0]) return;
  int ux = x + tft.textWidth(num) + 3;
  if (deg) {  // the fonts have no degree sign, so draw a small ring
    tft.drawCircle(ux + 3, base - fs->asc + 4, 3, DIM);
    tft.drawCircle(ux + 3, base - fs->asc + 4, 2, DIM);
    ux += 9;
  }
  tft.setFont(F_TEXT.f);
  tft.setTextColor(DIM);
  tft.drawString(unit, ux, base);
}

static void bar(int x, int y, int w, int h, float pct, uint16_t color, uint16_t track) {
  int fill = isnan(pct) ? 0 : (int)(constrain(pct, 0.0f, 100.0f) * w / 100.0f + 0.5f);
  uint32_t key = posKey(x, y);
  BCache &c = bcache[(x * 31 + y * 7) % NCACHE];
  if (c.key == key && c.fill == fill && c.color == color) return;
  c.key = key; c.fill = fill; c.color = color;
  tft.fillRoundRect(x, y, w, h, h / 2, track);
  if (fill > 0) tft.fillRoundRect(x, y, max(fill, (int)h), h, h / 2, color);
}

// Shorten a string until it fits.
static void fitText(char *s, const FontSpec &fs, int w) {
  tft.setFont(fs.f);
  while (strlen(s) > 1 && tft.textWidth(s) > w) s[strlen(s) - 1] = 0;
}

// Split a name over two lines at a space / punctuation, each at most w pixels wide.
static void wrap2(const char *name, const FontSpec &fs, int w, char *l1, char *l2, size_t n) {
  tft.setFont(fs.f);
  strlcpy(l1, name, n);
  l2[0] = 0;
  if (tft.textWidth(l1) <= w) return;

  int len = strlen(name), maxFit = 1;               // longest prefix that still fits on one line
  for (int i = 1; i <= len; i++) {
    char tmp[48];
    strlcpy(tmp, name, min((size_t)i + 1, sizeof tmp));
    if (tft.textWidth(tmp) > w) break;
    maxFit = i;
  }
  int cut = maxFit;                                 // prefer breaking at a separator or a camelCase boundary
  for (int j = maxFit; j > 0; j--)
    if (strchr(" :-_./", name[j]) || (j > 0 && isupper((unsigned char)name[j]) && islower((unsigned char)name[j - 1]))) { cut = j; break; }

  strlcpy(l1, name, min((size_t)cut + 1, n));
  const char *rest = name + cut;
  while (*rest && strchr(" :-_./", *rest)) rest++;
  strlcpy(l2, rest, n);
  fitText(l2, fs, w);
}

// ---------------------------------------------------------------- number formatting
static const char *num(char *buf, size_t n, float v, const char *fmt) {
  if (isnan(v)) strlcpy(buf, "--", n); else snprintf(buf, n, fmt, v);
  return buf;
}

// bytes -> "10.6" + "GB"   or   "449" + "MB"
static void bytesNum(char *numBuf, size_t n, const char **unit, float bytes) {
  if (isnan(bytes)) { strlcpy(numBuf, "--", n); *unit = "GB"; return; }
  if (bytes >= 1073741824.0f) { snprintf(numBuf, n, "%.1f", bytes / 1073741824.0f); *unit = "GB"; }
  else                        { snprintf(numBuf, n, "%.0f", bytes / 1048576.0f);    *unit = "MB"; }
}

static void gib(char *buf, size_t n, float bytes, const char *fmt = "%.1fG") {
  if (isnan(bytes)) strlcpy(buf, "--", n); else snprintf(buf, n, fmt, bytes / 1073741824.0f);
}

static void rate(char *out, size_t n, float b) {
  if (isnan(b)) strlcpy(out, "--", n);
  else if (b < 1024) snprintf(out, n, "%.0f B/s", b);
  else if (b < 1048576) snprintf(out, n, "%.0f KB/s", b / 1024);
  else snprintf(out, n, "%.1f MB/s", b / 1048576);
}

static void uptimeStr(char *out, size_t n, float s) {
  if (isnan(s)) { out[0] = 0; return; }
  uint32_t t = (uint32_t)s, d = t / 86400, h = (t / 3600) % 24, m = (t / 60) % 60;
  if (d) snprintf(out, n, "up %ud %02uh", (unsigned)d, (unsigned)h);
  else   snprintf(out, n, "up %uh %02um", (unsigned)h, (unsigned)m);
}

// ---------------------------------------------------------------- graphs (drawn into a sprite, then pushed)
// v[0..n) are the newest n samples; N is how many samples fill the full width, so a short history sits at the right.
static void graphDraw(lgfx::LGFX_Sprite &g, const float *v, int n, int N, float lo, float hi,
                      uint16_t line, uint16_t fillC, uint16_t bg, bool grid, int lm = 0) {
  int w = g.width(), h = g.height();
  g.fillSprite(bg);
  if (grid) for (int i = 1; i < 4; i++) g.drawFastHLine(lm, h * i / 4, w - lm, LINE);
  if (n < 1 || !(hi > lo)) return;
  float dx = N > 1 ? (float)(w - lm - 4) / (N - 1) : 0;
  int px = -1, py = 0;
  for (int i = 0; i < n; i++) {
    if (isnan(v[i])) { px = -1; continue; }
    int x = (int)((w - 3) - (n - 1 - i) * dx);
    float t = constrain((v[i] - lo) / (hi - lo), 0.0f, 1.0f);
    int y = (h - 3) - (int)(t * (h - 7));
    if (px >= 0) {
      for (int xx = px; xx <= x; xx++) {
        int yy = py + (y - py) * (xx - px) / max(1, x - px);
        g.drawFastVLine(xx, yy, h - 2 - yy, fillC);
      }
      g.drawLine(px, py, x, y, line);
      g.drawLine(px, py - 1, x, y - 1, line);
    } else {
      g.drawPixel(x, y, line);
    }
    px = x; py = y;
  }
}

// auto range for graphs: pads the data and keeps at least minSpan visible
static void autoRange(const float *v, int n, bool fromZero, float minSpan, float *lo, float *hi) {
  float mn = INFINITY, mx = -INFINITY;
  for (int i = 0; i < n; i++) if (!isnan(v[i])) { mn = min(mn, v[i]); mx = max(mx, v[i]); }
  if (mn > mx) { *lo = 0; *hi = minSpan; return; }
  if (fromZero) mn = 0;
  float span = max(mx - mn, minSpan);
  float mid = (mx + mn) / 2;
  *lo = fromZero ? 0 : mid - span * 0.6f;
  *hi = fromZero ? max(mx * 1.15f, minSpan) : mid + span * 0.6f;
}

// ---------------------------------------------------------------- small widgets
static bool inRect(int px, int py, int x, int y, int w, int h) {
  return px >= x && px < x + w && py >= y && py < y + h;
}

static void button(int x, int y, int w, int h, const char *label, uint16_t fill = CARD2, uint16_t fg = TEXT) {
  tft.fillRoundRect(x, y, w, h, 6, fill);
  tft.drawRoundRect(x, y, w, h, 6, LINE);
  tft.setFont(F_LABEL.f);
  tft.setTextColor(fg);
  tft.setTextDatum(lgfx::textdatum_t::baseline_center);
  tft.drawString(label, x + w / 2, y + h / 2 + F_LABEL.asc / 2);
}
