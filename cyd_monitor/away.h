#pragma once
// "Away mode": screen savers shown while the BC250 is switched off / unreachable.
// DVD logo, XP-style pipes, starfield, Matrix rain. All of them draw incrementally (only what moved),
// so there is no flicker and almost no CPU use.
#include "theme.h"

static const char *AWAY_NAMES[6] = {"DVD logo", "Pipes", "Starfield", "Matrix", "Cycle", "Screen off"};
enum { AW_DVD, AW_PIPES, AW_STARS, AW_MATRIX, AW_N, AW_CYCLE = 4, AW_OFF = 5 };   // 0..3 = one animation, 4 = rotate, 5 = backlight off

static int      gAwayMode = 0;            // persisted: index into AWAY_NAMES
static bool     gAway = false;            // away mode is on screen right now
static uint32_t gAwayPreviewUntil = 0;    // Settings -> Preview
static uint32_t gAwayWakeUntil = 0;       // tap while "Screen off": show the animation for a while
static int      awayAnim = -1;            // animation running now, -1 = none (screen off)
static uint32_t awayLast = 0, awaySwitchAt = 0, awayFrames = 0, awayCorners = 0;

static lgfx::LGFX_Sprite aSpr(&tft);      // the DVD logo, allocated only while that animation runs

static void fmtOffline(char *out, size_t n, uint32_t secs, bool preview) {
  if (preview) strlcpy(out, "preview", n);
  else if (secs < 60) snprintf(out, n, "offline %us", (unsigned)secs);
  else if (secs < 3600) snprintf(out, n, "offline %um %02us", (unsigned)(secs / 60), (unsigned)(secs % 60));
  else snprintf(out, n, "offline %uh %02um", (unsigned)(secs / 3600), (unsigned)((secs / 60) % 60));
}

// ================================================================ DVD logo
static const int DV_W = 118, DV_H = 50, DV_M = 4;          // logo box and the margin that erases its own trail
static struct { int x, y, vx, vy, ci; uint32_t flashUntil; } dvd;

static uint16_t dvdColor(int i) {
  static const Acc order[6] = {A_CPU, A_GPU, A_POWER, A_RAM, A_VRAM, A_GAME};
  return ACC[order[i % 6]];
}

static void dvdInit() {
  aSpr.setColorDepth(16);
  aSpr.createSprite(DV_W + 2 * DV_M, DV_H + 2 * DV_M);
  dvd.x = random(10, W - DV_W - 10); dvd.y = random(10, H - DV_H - 10);
  dvd.vx = random(2) ? 2 : -2; dvd.vy = random(2) ? 2 : -2;
  dvd.ci = random(6); dvd.flashUntil = 0;
}

static void dvdStep(uint32_t now, uint32_t offlineSecs, bool preview) {
  if (!aSpr.getBuffer()) return;
  int nx = dvd.x + dvd.vx, ny = dvd.y + dvd.vy;
  bool hx = false, hy = false;
  if (nx <= 0)            { nx = 0;            dvd.vx = abs(dvd.vx);  hx = true; }
  else if (nx >= W - DV_W) { nx = W - DV_W;     dvd.vx = -abs(dvd.vx); hx = true; }
  if (ny <= 0)            { ny = 0;            dvd.vy = abs(dvd.vy);  hy = true; }
  else if (ny >= H - DV_H) { ny = H - DV_H;     dvd.vy = -abs(dvd.vy); hy = true; }
  // a near miss of a corner is nudged into one, otherwise a 45-degree bounce almost never reaches it
  if (hx && !hy) { if (ny < 10) { ny = 0; dvd.vy = abs(dvd.vy); hy = true; } else if (ny > H - DV_H - 10) { ny = H - DV_H; dvd.vy = -abs(dvd.vy); hy = true; } }
  if (hy && !hx) { if (nx < 10) { nx = 0; dvd.vx = abs(dvd.vx); hx = true; } else if (nx > W - DV_W - 10) { nx = W - DV_W; dvd.vx = -abs(dvd.vx); hx = true; } }
  if (hx || hy) dvd.ci++;
  if (hx && hy) { awayCorners++; dvd.flashUntil = now + 2500; }
  dvd.x = nx; dvd.y = ny;

  bool flash = now < dvd.flashUntil;
  uint16_t col = (flash && ((now / 150) & 1)) ? TEXT : dvdColor(dvd.ci);
  aSpr.fillSprite(BG);
  aSpr.drawRoundRect(DV_M, DV_M, DV_W, DV_H, 9, col);
  aSpr.drawRoundRect(DV_M + 1, DV_M + 1, DV_W - 2, DV_H - 2, 8, col);
  const FontSpec &fs = textW(F_BIG, "BC250") <= DV_W - 14 ? F_BIG : F_MID;
  aSpr.setFont(fs.f);
  aSpr.setTextColor(col);
  aSpr.setTextDatum(lgfx::textdatum_t::baseline_center);
  aSpr.drawString("BC250", DV_M + DV_W / 2, DV_M + 32);
  char b[24];
  if (flash) snprintf(b, sizeof b, "CORNER HIT #%u!", (unsigned)awayCorners);
  else fmtOffline(b, sizeof b, offlineSecs, preview);
  aSpr.setFont(F_SMALL.f);
  aSpr.setTextColor(flash ? TEXT : DIM);
  aSpr.drawString(b, DV_M + DV_W / 2, DV_M + 45);
  aSpr.pushSprite(dvd.x - DV_M, dvd.y - DV_M);
}

// ================================================================ Pipes (the Windows XP screen saver, in 2D)
static const int PP_C = 10, PP_W = 32, PP_H = 24;          // 10 px cells over the whole screen
static uint8_t ppOcc[PP_W * PP_H];
struct Pipe { int8_t x, y, dx, dy; uint16_t base, hi, lo; bool alive; };
static Pipe pipes[3];
static int ppFilled = 0;
static uint32_t ppRestartAt = 0;

static bool ppFree(int x, int y) { return x >= 0 && x < PP_W && y >= 0 && y < PP_H && !ppOcc[y * PP_W + x]; }

static void ppBall(int x, int y, const Pipe &p) {
  int cx = x * PP_C + PP_C / 2, cy = y * PP_C + PP_C / 2;
  tft.fillCircle(cx, cy, 5, p.lo);
  tft.fillCircle(cx, cy, 4, p.base);
  tft.fillCircle(cx - 1, cy - 1, 2, p.hi);
}

static void ppTube(int x0, int y0, int x1, int y1, const Pipe &p) {
  int cx0 = x0 * PP_C + PP_C / 2, cy0 = y0 * PP_C + PP_C / 2, cx1 = x1 * PP_C + PP_C / 2, cy1 = y1 * PP_C + PP_C / 2;
  if (cy0 == cy1) {                                      // horizontal: highlight on top
    int xa = min(cx0, cx1), len = abs(cx1 - cx0) + 1;
    tft.fillRect(xa, cy0 - 4, len, 9, p.lo);
    tft.fillRect(xa, cy0 - 3, len, 6, p.base);
    tft.fillRect(xa, cy0 - 3, len, 2, p.hi);
  } else {                                               // vertical: highlight on the left
    int ya = min(cy0, cy1), len = abs(cy1 - cy0) + 1;
    tft.fillRect(cx0 - 4, ya, 9, len, p.lo);
    tft.fillRect(cx0 - 3, ya, 6, len, p.base);
    tft.fillRect(cx0 - 3, ya, 2, len, p.hi);
  }
}

static void ppSpawn(Pipe &p) {
  static const uint8_t pal[8][3] = {{255, 70, 70}, {255, 170, 40}, {250, 230, 60}, {80, 220, 100},
                                    {60, 200, 240}, {90, 120, 255}, {200, 100, 255}, {255, 110, 190}};
  for (int tries = 0; tries < 40; tries++) {
    int x = random(PP_W), y = random(PP_H);
    if (!ppFree(x, y)) continue;
    const uint8_t *c = pal[random(8)];
    p.base = C565(c[0], c[1], c[2]);
    p.hi = mix565(p.base, 0xFFFF, 120);
    p.lo = mix565(p.base, 0x0000, 110);
    static const int8_t d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    int k = random(4);
    p.x = x; p.y = y; p.dx = d[k][0]; p.dy = d[k][1]; p.alive = true;
    ppOcc[y * PP_W + x] = 1; ppFilled++;
    ppBall(x, y, p);
    return;
  }
  p.alive = false;
}

static void ppReset() {
  memset(ppOcc, 0, sizeof ppOcc);
  ppFilled = 0;
  tft.fillScreen(BG);
  for (auto &p : pipes) ppSpawn(p);
}

static void ppStep(uint32_t now) {
  if (ppRestartAt) { if (now >= ppRestartAt) { ppRestartAt = 0; ppReset(); } return; }
  bool any = false;
  for (auto &p : pipes) {
    if (!p.alive) { if (ppFilled < PP_W * PP_H * 45 / 100) ppSpawn(p); if (!p.alive) continue; any = true; continue; }
    any = true;
    int8_t fx = p.dx, fy = p.dy, lx = -p.dy, ly = p.dx, rx = p.dy, ry = -p.dx;
    int8_t order[3][2] = {{fx, fy}, {lx, ly}, {rx, ry}};
    if (random(100) < 28) { int s = random(2) + 1; int8_t t0 = order[0][0], t1 = order[0][1]; order[0][0] = order[s][0]; order[0][1] = order[s][1]; order[s][0] = t0; order[s][1] = t1; }   // turn sometimes
    bool moved = false;
    for (int k = 0; k < 3 && !moved; k++) {
      int nx = p.x + order[k][0], ny = p.y + order[k][1];
      if (!ppFree(nx, ny)) continue;
      ppTube(p.x, p.y, nx, ny, p);
      ppOcc[ny * PP_W + nx] = 1; ppFilled++;
      ppBall(nx, ny, p);
      p.x = nx; p.y = ny; p.dx = order[k][0]; p.dy = order[k][1];
      moved = true;
    }
    if (!moved) p.alive = false;
  }
  if (!any || ppFilled >= PP_W * PP_H * 55 / 100) ppRestartAt = now + 1500;      // full: pause, then start over
}

// ================================================================ Starfield
static const int ST_N = 64;
static struct Star { float x, y, z; int16_t ax, ay, bx, by; } stars[ST_N];
static const float ST_ZMAX = 16.0f, ST_SPEED = 0.14f;

static void starRespawn(Star &s) {
  s.x = random(-1000, 1001) / 1000.0f; s.y = random(-1000, 1001) / 1000.0f;
  s.z = random(40, (int)(ST_ZMAX * 10)) / 10.0f;
  s.ax = s.ay = s.bx = s.by = -1;
}

static void starStep() {
  for (auto &s : stars) {
    if (s.ax >= 0) tft.drawLine(s.ax, s.ay, s.bx, s.by, BG);          // erase last frame's streak
    s.z -= ST_SPEED;
    float zt = s.z + ST_SPEED * 1.8f;                                  // tail of the streak
    int nx = W / 2 + (int)(s.x * 160.0f / s.z), ny = H / 2 + (int)(s.y * 120.0f / s.z);
    int tx = W / 2 + (int)(s.x * 160.0f / zt), ty = H / 2 + (int)(s.y * 120.0f / zt);
    if (s.z < 0.5f || nx < 0 || nx >= W || ny < 0 || ny >= H) { starRespawn(s); continue; }
    int b = constrain((int)(255.0f * (1.0f - s.z / ST_ZMAX)) + 40, 40, 255);
    tft.drawLine(tx, ty, nx, ny, C565(b, b, constrain(b + 30, 0, 255)));
    s.ax = tx; s.ay = ty; s.bx = nx; s.by = ny;
  }
}

// ================================================================ Matrix rain
static const int MX_C = 40, MX_R = 30;                                 // 8 px cells
static struct MCol { int16_t row; uint8_t spd, cnt, len; } mcols[MX_C];
static const char MX_CH[] = "0123456789ABCDEFXZ#$%&*+<>=?";

static void mxCell(int c, int r, uint16_t col) {
  if (r < 0 || r >= MX_R) return;
  char s[2] = {MX_CH[random(sizeof(MX_CH) - 1)], 0};
  tft.setTextColor(col, BG);
  tft.drawString(s, c * 8 + 1, r * 8);
}
static void mxReset(MCol &m) { m.row = -random(0, 24); m.spd = 1 + random(3); m.cnt = 0; m.len = 6 + random(14); }

static void mxInit() {
  tft.fillScreen(BG);
  for (auto &m : mcols) mxReset(m);
}

static void mxStep() {
  uint16_t head = mix565(ACC[A_RAM], 0xFFFF, 90), body = ACC[A_RAM], tail = mix565(ACC[A_RAM], BG, 120);
  tft.setFont(&lgfx::fonts::Font0);
  tft.setTextDatum(lgfx::textdatum_t::top_left);
  for (int c = 0; c < MX_C; c++) {
    MCol &m = mcols[c];
    if (++m.cnt < m.spd) continue;
    m.cnt = 0; m.row++;
    mxCell(c, m.row, head);
    mxCell(c, m.row - 1, body);
    mxCell(c, m.row - 4, tail);
    int er = m.row - m.len;
    if (er >= 0 && er < MX_R) tft.fillRect(c * 8, er * 8, 8, 8, BG);
    if (er > MX_R) mxReset(m);
  }
}

// ================================================================ control
static void awayStop() { if (aSpr.getBuffer()) aSpr.deleteSprite(); awayAnim = -1; }

static void awayBegin(int anim) {
  awayStop();
  awayAnim = anim;
  awayLast = 0;
  tft.fillScreen(BG);
  switch (anim) {
    case AW_DVD:    dvdInit(); break;
    case AW_PIPES:  ppRestartAt = 0; ppReset(); break;
    case AW_STARS:  for (auto &s : stars) starRespawn(s); break;
    case AW_MATRIX: mxInit(); break;
  }
}

// called when away mode starts; `showing` = false for the "Screen off" setting
static void awayEnter(uint32_t now) {
  gAway = true;
  awayBegin(gAwayMode == AW_CYCLE ? random(AW_N) : (gAwayMode < AW_N ? gAwayMode : AW_DVD));
  awaySwitchAt = now + 75000;
}

static void awayLeave() { gAway = false; awayStop(); }

static void awayNext(uint32_t now) {
  awayBegin((awayAnim + 1) % AW_N);
  awaySwitchAt = now + 75000;
}

static void awayStep(uint32_t now, uint32_t offlineSecs, bool preview) {
  if (awayAnim < 0) return;
  if (gAwayMode == AW_CYCLE && now > awaySwitchAt) { awayBegin((awayAnim + 1 + random(AW_N - 1)) % AW_N); awaySwitchAt = now + 75000; }
  static const uint16_t interval[AW_N] = {33, 70, 33, 66};
  if (now - awayLast < interval[awayAnim]) return;
  awayLast = now;
  awayFrames++;
  switch (awayAnim) {
    case AW_DVD:    dvdStep(now, offlineSecs, preview); break;
    case AW_PIPES:  ppStep(now); break;
    case AW_STARS:  starStep(); break;
    case AW_MATRIX: mxStep(); break;
  }
}
