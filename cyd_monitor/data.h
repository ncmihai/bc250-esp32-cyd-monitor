#pragma once
#include <WiFi.h>
#include <HTTPClient.h>
#include "config.h"

// ---------------------------------------------------------------- key=value store
// Every endpoint returns "key=value" lines; this keeps the latest parse of one endpoint.
struct KV    { char k[24]; char v[36]; };
struct Store { KV e[64]; int n = 0; };

static float kvF(const Store &s, const char *k) {
  for (int i = 0; i < s.n; i++) if (!strcmp(s.e[i].k, k)) return atof(s.e[i].v);
  return NAN;
}
static const char *kvS(const Store &s, const char *k, const char *def = "") {
  for (int i = 0; i < s.n; i++) if (!strcmp(s.e[i].k, k)) return s.e[i].v;
  return def;
}

static void kvParse(const char *body, Store &s) {
  s.n = 0;
  const char *p = body;
  while (*p && s.n < 64) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    const char *eq = (const char *)memchr(p, '=', len);
    if (eq && eq > p) {
      KV &e = s.e[s.n++];
      size_t kl = min((size_t)(eq - p), sizeof(e.k) - 1);
      memcpy(e.k, p, kl); e.k[kl] = 0;
      size_t vl = min(len - (size_t)(eq - p) - 1, sizeof(e.v) - 1);
      memcpy(e.v, eq + 1, vl); e.v[vl] = 0;
      while (vl && (e.v[vl - 1] == '\r')) e.v[--vl] = 0;
    }
    p += len + (nl ? 1 : 0);
  }
}

// ---------------------------------------------------------------- history store
static const int HMAX = 160;
struct Hist    { char key[18]; float v[HMAX]; int n; };
struct HistSet { Hist h[3]; int count = 0; };

static void histParse(const char *body, HistSet &hs) {
  hs.count = 0;
  const char *p = body;
  while (*p && hs.count < 3) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    const char *eq = (const char *)memchr(p, '=', len);
    if (eq && eq > p) {
      Hist &h = hs.h[hs.count++];
      size_t kl = min((size_t)(eq - p), sizeof(h.key) - 1);
      memcpy(h.key, p, kl); h.key[kl] = 0;
      h.n = 0;
      const char *q = eq + 1, *end = p + len;
      while (q <= end && h.n < HMAX) {
        const char *comma = (const char *)memchr(q, ',', end - q);
        const char *stop = comma ? comma : end;
        h.v[h.n++] = (stop == q) ? NAN : (float)atof(q);
        if (!comma) break;
        q = comma + 1;
      }
    }
    p += len + (nl ? 1 : 0);
  }
}

// ---------------------------------------------------------------- shared state (net task writes, UI reads)
static SemaphoreHandle_t gMtx;
static Store   gMain, gExtra;
static HistSet gHist;
// Version counters: bumped on every update, kept OUT of the structs so that copying a struct can never reset them.
static uint32_t vMain = 0, vExtra = 0, vHist = 0;
static char    gReqExtra[8] = "";   // "cpu" | "gpu" | "mem" | "ai" | ""  : which /x/<name>.txt the open page wants
static char    gReqHist[24] = "";   // metric to fetch history for, "" = none
static int     gReqStep = 1, gReqN = 60;
static volatile bool     gLinkOk = false, gWake = false;
static volatile uint32_t gLastOk = 0;
static char    gErr[48] = "";

static void lock()   { xSemaphoreTake(gMtx, portMAX_DELAY); }
static void unlock() { xSemaphoreGive(gMtx); }

// What the open page needs besides the always-fetched /stats.txt. Clears stale data of the previous page.
static void netRequest(const char *extra, const char *hist, int step, int n) {
  lock();
  strlcpy(gReqExtra, extra, sizeof gReqExtra);
  strlcpy(gReqHist, hist, sizeof gReqHist);
  gReqStep = step; gReqN = n;
  gExtra.n = 0; vExtra++;
  gHist.count = 0; vHist++;
  unlock();
  gWake = true;
}

static char gBody[2048];

static bool httpGet(const char *path) {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(1200);
  http.setTimeout(1500);
  if (!http.begin(client, STATS_HOST, STATS_PORT, path)) { strlcpy(gErr, "begin failed", sizeof gErr); return false; }
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    if (code < 0) snprintf(gErr, sizeof gErr, "%s", HTTPClient::errorToString(code).c_str());
    else snprintf(gErr, sizeof gErr, "HTTP %d", code);
    http.end();
    return false;
  }
  int len = http.getSize(), total = 0;
  WiFiClient *s = http.getStreamPtr();
  uint32_t t0 = millis();
  while ((http.connected() || s->available()) && total < (int)sizeof(gBody) - 1 && millis() - t0 < 1500) {
    int avail = s->available();
    if (avail) total += s->readBytes(gBody + total, min(avail, (int)sizeof(gBody) - 1 - total));
    else if (len >= 0 && total >= len) break;
    else delay(2);
  }
  gBody[total] = 0;
  http.end();
  return total > 0;
}

static void netTask(void *) {
  static Store   tmp;
  static HistSet htmp;
  int fails = 0;
  for (;;) {
    uint32_t t0 = millis();
    if (WiFi.status() == WL_CONNECTED) {
      char extra[8], hist[24], path[80];
      int step, n;
      lock(); strlcpy(extra, gReqExtra, sizeof extra); strlcpy(hist, gReqHist, sizeof hist); step = gReqStep; n = gReqN; unlock();

      if (httpGet("/stats.txt")) {
        kvParse(gBody, tmp);
        lock(); memcpy(&gMain, &tmp, sizeof gMain); vMain++; unlock();
        gLastOk = millis(); fails = 0; gLinkOk = true;

        if (extra[0]) {
          snprintf(path, sizeof path, "/x/%s.txt", extra);
          if (httpGet(path)) {
            kvParse(gBody, tmp);
            lock();
            if (!strcmp(extra, gReqExtra)) { memcpy(&gExtra, &tmp, sizeof gExtra); vExtra++; }
            unlock();
          }
        }
        if (hist[0]) {
          snprintf(path, sizeof path, "/hist?m=%s&step=%d&n=%d", hist, step, n);
          if (httpGet(path)) {
            histParse(gBody, htmp);
            lock();
            if (!strcmp(hist, gReqHist)) { memcpy(&gHist, &htmp, sizeof gHist); vHist++; }
            unlock();
          }
        }
      } else if (++fails >= 2) {
        gLinkOk = false;
      }
    } else {
      gLinkOk = false;
    }
    while (millis() - t0 < 1000 && !gWake) vTaskDelay(pdMS_TO_TICKS(20));
    gWake = false;
  }
}

static void netStart() {
  gMtx = xSemaphoreCreateMutex();
  xTaskCreatePinnedToCore(netTask, "net", 8192, nullptr, 1, nullptr, 0);
}
