#include "app.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <Update.h>
#include "certs.h"
#include <Sgp4.h>

volatile bool wifiUp = false, apMode = false;
String ipStr;
LiveData live;
SemaphoreHandle_t dataLock;

static WebServer server(80);
static DNSServer dns;
static String scanOptions;
static volatile int haToggleReq = -1;
static volatile bool refreshReq = false;
static volatile bool placaReq = false;
static bool everUp = false;

void hermesBegin();
void hermesLoop();


void netRequestHaToggle(int i) { haToggleReq = i; }
void netRequestRefresh() { refreshReq = true; }
void netRequestPlaca() { placaReq = true; }

// ---------- helpers ----------
static String urlEncode(const String& s) {
  String o; char b[4];
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else { snprintf(b, sizeof(b), "%%%02X", c); o += b; }
  }
  return o;
}

static String esc(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += "&amp;"; else if (c == '<') o += "&lt;"; else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;"; else o += c;
  }
  return o;
}

extern volatile uint32_t netHeartbeat;
static uint32_t demoSince = 0;
static void demoFill();
static int lastHttp = 0;
static bool httpGet(const String& url, String& body, const String& bearer = "", const char* hName = nullptr, const String& hVal = "", const char* accept = "application/json") {
  HTTPClient http;
  WiFiClientSecure sec;
  WiFiClient plain;
  bool ok;
  if (url.startsWith("https")) { sec.setCACert(ROOT_CAS); ok = http.begin(sec, url); }
  else ok = http.begin(plain, url);
  if (!ok) return false;
  http.setTimeout(8000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("Accept", accept);
  if (bearer.length()) http.addHeader("Authorization", "Bearer " + bearer);
  if (hName && hVal.length()) http.addHeader(hName, hVal);
  int code = http.GET();
  lastHttp = code;
  if (code == 200) body = http.getString();
  else {
    int q = url.indexOf('?');
    char tls[64] = "";
    if (code < 0 && url.startsWith("https")) sec.lastError(tls, sizeof(tls));
    logf("[net] GET %s -> %d %s (heap %u, bloco %u)", url.substring(0, q > 0 ? min(q, 60) : 60).c_str(), code, tls,
         ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  }
  http.end();
  vTaskDelay(pdMS_TO_TICKS(20));
  return code == 200;
}

static bool httpPost(const String& url, const String& payload, const String& bearer) {
  HTTPClient http;
  WiFiClientSecure sec;
  WiFiClient plain;
  bool ok;
  if (url.startsWith("https")) { sec.setCACert(ROOT_CAS); ok = http.begin(sec, url); }
  else ok = http.begin(plain, url);
  if (!ok) return false;
  http.setTimeout(8000);
  http.addHeader("Content-Type", "application/json");
  if (bearer.length()) http.addHeader("Authorization", "Bearer " + bearer);
  int code = http.POST(payload);
  http.end();
  return code >= 200 && code < 300;
}

static void lock() { xSemaphoreTake(dataLock, portMAX_DELAY); }
static void unlock() { xSemaphoreGive(dataLock); }

// ---------- data sources ----------
static String fngPt(const String& en) {
  if (en == "Extreme Fear") return "Medo extremo";
  if (en == "Fear") return "Medo";
  if (en == "Neutral") return "Neutro";
  if (en == "Greed") return "Ganância";
  if (en == "Extreme Greed") return "Ganância extrema";
  return en;
}

static void fetchBtc() {
  // public, keyless APIs: CoinGecko (prices), mempool.space (fees), alternative.me (fear & greed)
  String b;
  JsonDocument d;
  bool any = false;
  if (httpGet("https://api.coingecko.com/api/v3/simple/price?ids=bitcoin,ethereum,solana&vs_currencies=usd&include_24hr_change=true", b) &&
      !deserializeJson(d, b)) {
    lock();
    live.btc = d["bitcoin"]["usd"] | 0.0f; live.btcChg = d["bitcoin"]["usd_24h_change"] | 0.0f;
    live.eth = d["ethereum"]["usd"] | 0.0f; live.ethChg = d["ethereum"]["usd_24h_change"] | 0.0f;
    live.sol = d["solana"]["usd"] | 0.0f; live.solChg = d["solana"]["usd_24h_change"] | 0.0f;
    live.btcOk = live.btc > 0;
    any = live.btcOk;
    unlock();
  }
  if (httpGet("https://mempool.space/api/v1/fees/recommended", b) && !deserializeJson(d, b)) {
    lock();
    live.feeSlow = d["hourFee"] | 0; live.feeStd = d["halfHourFee"] | 0; live.feeFast = d["fastestFee"] | 0;
    unlock();
  }
  if (httpGet("https://api.alternative.me/fng/?limit=1", b) && !deserializeJson(d, b)) {
    lock();
    live.fng = String((const char*)(d["data"][0]["value"] | "-1")).toInt();
    live.fngLabel = fngPt(String((const char*)(d["data"][0]["value_classification"] | "")));
    unlock();
  }
  if (any) { time_t now; time(&now); live.btcAt = now; }
  logf("[net] btc %.0f fees %d/%d/%d fng %d\n", live.btc, live.feeSlow, live.feeStd, live.feeFast, live.fng);
}

static void geocode() {
  if (!cfg.city.length()) return;   // no city set yet
  String name = cfg.city;
  int comma = name.indexOf(',');
  if (comma > 0) name = name.substring(0, comma);
  String b;
  JsonDocument d;
  if (httpGet("https://geocoding-api.open-meteo.com/v1/search?count=1&language=pt&name=" + urlEncode(name), b) && !deserializeJson(d, b)) {
    if (d["results"][0]["latitude"].is<float>()) {
      cfg.lat = d["results"][0]["latitude"]; cfg.lon = d["results"][0]["longitude"];
      configSave();
      logf("[net] geocoded %s -> %.4f,%.4f\n", name.c_str(), cfg.lat, cfg.lon);
    }
  }
}

// no city set: approximate place from the public IP (city level, can be off by a region)
static float ipLat = NAN, ipLon = NAN;
static String ipCity;
static bool ipLocate() {
  String b;
  JsonDocument d;
  if (httpGet("https://ipwho.is/", b) && !deserializeJson(d, b) && (d["success"] | false)) {
    ipLat = d["latitude"] | NAN; ipLon = d["longitude"] | NAN; ipCity = String((const char*)(d["city"] | ""));
  } else if (httpGet("http://ip-api.com/json/?fields=status,city,lat,lon", b) && !deserializeJson(d, b) && String((const char*)(d["status"] | "")) == "success") {
    ipLat = d["lat"] | NAN; ipLon = d["lon"] | NAN; ipCity = String((const char*)(d["city"] | ""));
  }
  logf("[net] located by IP: %s %.3f,%.3f\n", ipCity.c_str(), ipLat, ipLon);
  return !isnan(ipLat);
}

static const char* DOW[7] = {"dom.", "seg.", "ter.", "qua.", "qui.", "sex.", "sáb."};

static void fetchWeather() {
  float lat = cfg.lat, lon = cfg.lon;
  bool byIp = false;
  if (cfg.city.length()) {
    if (isnan(cfg.lat) || isnan(cfg.lon)) geocode();
    lat = cfg.lat; lon = cfg.lon;
  } else {
    if (isnan(ipLat) && !ipLocate()) return;
    lat = ipLat; lon = ipLon; byIp = true;
  }
  if (isnan(lat)) return;
  lock(); live.wxByIp = byIp; live.wxPlace = byIp ? ipCity : cfg.city; unlock();
  char url[360];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m"
           "&daily=temperature_2m_max,temperature_2m_min,weather_code&timezone=America%%2FSao_Paulo&forecast_days=4",
           lat, lon);
  String b;
  JsonDocument d;
  if (!httpGet(url, b) || deserializeJson(d, b)) return;
  lock();
  live.wxTemp = d["current"]["temperature_2m"] | 0.0f;
  live.wxHum = d["current"]["relative_humidity_2m"] | 0;
  live.wxWind = d["current"]["wind_speed_10m"] | 0.0f;
  live.wxCode = d["current"]["weather_code"] | 0;
  for (int i = 0; i < 4; i++) {
    live.dMax[i] = d["daily"]["temperature_2m_max"][i] | 0.0f;
    live.dMin[i] = d["daily"]["temperature_2m_min"][i] | 0.0f;
    live.dCode[i] = d["daily"]["weather_code"][i] | 0;
    const char* t = d["daily"]["time"][i] | "";
    struct tm tmv = {};
    if (strlen(t) >= 10) {
      tmv.tm_year = atoi(t) - 1900; tmv.tm_mon = atoi(t + 5) - 1; tmv.tm_mday = atoi(t + 8); tmv.tm_hour = 12;
      mktime(&tmv);
      live.dDay[i] = DOW[tmv.tm_wday % 7];
    }
  }
  live.wxOk = true;
  time_t now; time(&now); live.wxAt = now;
  unlock();
  logf("[net] weather %.1fC code %d\n", live.wxTemp, live.wxCode);
}

static void fetchSocial() {
  if (!cfg.n8nUrl.length()) return;
  String b;
  JsonDocument d;
  if (!httpGet(cfg.n8nUrl, b) || deserializeJson(d, b)) return;
  lock();
  live.socTotal = d["total"] | -1L;
  live.socYt = d["youtube"] | -1L; live.socIg = d["instagram"] | -1L; live.socTt = d["tiktok"] | -1L;
  live.socX = d["x"] | -1L; live.socTg = d["telegram"] | -1L;
  live.socTh = d["threads"] | -1L; live.socFb = d["facebook"] | -1L;
  if (d["ok"].is<bool>() && !d["ok"].as<bool>()) { live.socOk = false; unlock(); return; }
  if (live.socTotal < 0) {
    long t = 0; long v[7] = {live.socYt, live.socIg, live.socTt, live.socX, live.socTg, live.socTh, live.socFb};
    for (long x : v) if (x > 0) t += x;
    live.socTotal = t > 0 ? t : -1;
  }
  live.socOk = true;
  time_t now; time(&now); live.socAt = now;
  unlock();
}

static void fetchHa() {
  if (!cfg.haUrl.length() || !cfg.haToken.length()) return;
  String base = cfg.haUrl;
  if (base.endsWith("/")) base.remove(base.length() - 1);
  bool anyOk = false;
  for (int i = 0; i < 4; i++) {
    if (!cfg.haEnt[i].length()) { live.haState[i] = -1; continue; }
    String b;
    JsonDocument d;
    if (httpGet(base + "/api/states/" + cfg.haEnt[i], b, cfg.haToken) && !deserializeJson(d, b)) {
      String st = d["state"] | "";
      lock();
      live.haState[i] = (st == "on" || st == "open" || st == "playing" || st == "home") ? 1 : (st == "off" || st == "closed" ? 0 : -1);
      unlock();
      anyOk = true;
    }
  }
  lock(); live.haOk = anyOk; live.haErr = anyOk ? "" : "sem resposta do Home Assistant"; unlock();
}

static void haToggle(int i) {
  if (i < 0 || i > 3 || !cfg.haEnt[i].length()) return;
  String base = cfg.haUrl;
  if (base.endsWith("/")) base.remove(base.length() - 1);
  String body = "{\"entity_id\":\"" + cfg.haEnt[i] + "\"}";
  if (httpPost(base + "/api/services/homeassistant/toggle", body, cfg.haToken)) {
    lock(); if (live.haState[i] >= 0) live.haState[i] = !live.haState[i]; unlock();
  }
  delay(400);
  fetchHa();
}

// ---------- ISO-8601 -> epoch ----------
static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}
uint32_t parseIso(const char* s) {
  if (!s || !*s) return 0;
  int Y, M, D, h = 0, mi = 0, se = 0;
  if (sscanf(s, "%d-%d-%d", &Y, &M, &D) != 3) return 0;
  long off = -3 * 3600;                 // no time zone given: Brasília
  const char* t = strchr(s, 'T');
  if (!t) t = strchr(s, ' ');
  if (t) {
    sscanf(t + 1, "%d:%d:%d", &h, &mi, &se);
    const char* z = strpbrk(t + 1, "+-Z");
    if (z && *z == 'Z') off = 0;
    else if (z) { int oh = 0, om = 0; sscanf(z + 1, "%d:%d", &oh, &om); off = (oh * 3600L + om * 60L) * (*z == '-' ? -1 : 1); }
  }
  int64_t e = daysFromCivil(Y, M, D) * 86400LL + h * 3600LL + mi * 60LL + se - off;
  return e > 0 ? (uint32_t)e : 0;
}

// ---------- summary feed published by your server (docs/feed.md) ----------
static void fetchPlaca() {
  if (!cfg.feedUrl.length()) { lock(); live.plErr = "feed não configurado"; unlock(); return; }
  String b;
  if (!httpGet(cfg.feedUrl, b, cfg.feedToken)) {
    lock(); live.plErr = (lastHttp == 401 || lastHttp == 403) ? "feed: token ausente ou errado" : "feed sem resposta"; unlock(); return;
  }
  JsonDocument d;
  if (deserializeJson(d, b)) { lock(); live.plErr = "resposta inválida"; unlock(); return; }
  // accepts the summary itself or wrapped as {"ok":true,"dados":{...}}
  JsonObject p = d["dados"].is<JsonObject>() ? d["dados"].as<JsonObject>() : d.as<JsonObject>();
  if ((d["ok"].is<bool>() && !d["ok"].as<bool>()) || (p["pendente"] | false)) { lock(); live.plErr = "servidor ainda não publicou"; live.plOk = false; unlock(); return; }
  lock();
  live.saude = String((const char*)(p["saude"] | ""));
  if (!cfg.icalUrl.length()) live.agN = 0;   // with an iCal link the agenda comes from there
  if (!cfg.icalUrl.length()) for (JsonObject a : p["agenda"].as<JsonArray>()) {
    if (live.agN >= 3) break;
    live.agT[live.agN] = String((const char*)(a["t"] | ""));
    live.agQ[live.agN] = parseIso(a["quando"] | "");
    live.agDia[live.agN] = a["dia_todo"] | false;
    live.agN++;
  }
  live.nwN = 0;
  for (JsonObject n : p["manchetes"].as<JsonArray>()) {
    if (live.nwN >= 5) break;
    live.nwT[live.nwN] = String((const char*)(n["t"] | ""));
    live.nwF[live.nwN] = String((const char*)(n["fonte"] | ""));
    live.nwN++;
  }
  live.mqN = 0;
  for (JsonObject m : p["maquinas"].as<JsonArray>()) {
    if (live.mqN >= 5) break;
    int k = live.mqN;
    live.mqNome[k] = String((const char*)(m["n"] | "?"));
    live.mqCpu[k] = m["cpu"] | -1; live.mqRam[k] = m["ram"] | -1; live.mqTemp[k] = m["temp"] | -1;
    live.mqOn[k] = m["on"] | false;
    live.mqDisk[k] = m["disco"] | -1;
    live.mqAge[k] = m["idade_s"] | 0;
    live.mqN++;
  }
  live.iaCusto = p["ia"]["custo_usd"] | -1.0f;
  live.iaTokens = p["ia"]["tokens"] | -1.0;
  live.iaN = 0;
  for (JsonObject m : p["ia"]["por_maquina"].as<JsonArray>()) {
    if (live.iaN >= 4) break;
    live.iaNome[live.iaN] = String((const char*)(m["n"] | "?"));
    live.iaC[live.iaN] = m["custo"] | 0.0f;
    live.iaN++;
  }
  live.plOk = true; live.plErr = "";
  uint32_t g = parseIso(p["gerado_em"] | "");
  if (!g) { time_t now; time(&now); g = now; }
  live.plAt = g;
  unlock();
  logf("[net] placa saude=%s agenda=%d manchetes=%d maquinas=%d ia=%.2f\n", live.saude.c_str(), live.agN, live.nwN, live.mqN, live.iaCusto);
}

// ---------- Pi-hole v6 (local) ----------
// POST /api/auth {"password"} -> session.sid, then GET /api/stats/summary with X-FTL-SID.
// The sid is reused until Pi-hole rejects it, so we don't burn API seats.
static String phSid;
static String phBase() { String b = cfg.phUrl; if (b.endsWith("/")) b.remove(b.length() - 1); if (b.endsWith("/admin")) b.remove(b.length() - 6); return b; }
static bool phAuth() {
  if (!cfg.phPass.length()) { phSid = ""; return true; }  // no password set on the Pi-hole
  HTTPClient http; WiFiClient c;
  if (!http.begin(c, phBase() + "/api/auth")) return false;
  http.setTimeout(6000);
  http.addHeader("Content-Type", "application/json");
  JsonDocument q; q["password"] = cfg.phPass; String body; serializeJson(q, body);
  int code = http.POST(body);
  String r = code == 200 ? http.getString() : "";
  http.end();
  JsonDocument d;
  if (code != 200 || deserializeJson(d, r) || !(d["session"]["valid"] | false)) {
    logf("[net] pihole auth -> %d\n", code);
    lock(); live.phErr = code == 401 ? "senha do Pi-hole recusada" : "Pi-hole sem resposta"; unlock();
    return false;
  }
  phSid = String((const char*)(d["session"]["sid"] | ""));
  return true;
}
static void fetchPihole() {
  if (!cfg.phUrl.length() || !MOD_ON(M_PIHOLE)) return;
  for (int attempt = 0; attempt < 2; attempt++) {
    if (attempt == 1 || (cfg.phPass.length() && !phSid.length())) { if (!phAuth()) return; }
    HTTPClient http; WiFiClient c;
    if (!http.begin(c, phBase() + "/api/stats/summary")) return;
    http.setTimeout(6000);
    if (phSid.length()) http.addHeader("X-FTL-SID", phSid);
    int code = http.GET();
    String r = code == 200 ? http.getString() : "";
    http.end();
    if (code == 401) { phSid = ""; continue; }
    JsonDocument d;
    if (code != 200 || deserializeJson(d, r)) {
      logf("[net] pihole summary -> %d\n", code);
      lock(); live.phOk = false; live.phErr = "Pi-hole sem resposta"; unlock();
      return;
    }
    lock();
    live.phTotal = d["queries"]["total"] | -1L;
    live.phBlocked = d["queries"]["blocked"] | -1L;
    live.phPct = d["queries"]["percent_blocked"] | 0.0f;
    live.phClients = d["clients"]["active"] | -1;
    live.phDomains = d["gravity"]["domains_being_blocked"] | -1L;
    live.phOk = live.phTotal >= 0; live.phErr = "";
    time_t now; time(&now); live.phAt = now;
    unlock();
    logf("[net] pihole %ld queries, %ld blocked (%.1f%%)\n", live.phTotal, live.phBlocked, live.phPct);
    return;
  }
}

// ---------- Wi-Fi: up to 3 saved networks, joins the strongest one in range ----------
static uint32_t lastJoin = 0;
static void joinBest() {
  lastJoin = millis();
  const String* ss[3] = {&cfg.ssid, &cfg.ssid2, &cfg.ssid3};
  const String* pw[3] = {&cfg.wpass, &cfg.wpass2, &cfg.wpass3};
  int best = -1, bestRssi = -1000;
  int n = WiFi.scanNetworks();
  for (int i = 0; i < n; i++)
    for (int k = 0; k < 3; k++)
      if (ss[k]->length() && WiFi.SSID(i) == *ss[k] && WiFi.RSSI(i) > bestRssi) { best = k; bestRssi = WiFi.RSSI(i); }
  WiFi.scanDelete();
  if (best < 0) { for (int k = 0; k < 3; k++) if (ss[k]->length()) { best = k; break; } }
  if (best < 0) return;
  logf("[net] joining %s (rssi %d)\n", ss[best]->c_str(), bestRssi);
  WiFi.begin(ss[best]->c_str(), pw[best]->c_str());
}
static bool anySsid() { return cfg.ssid.length() || cfg.ssid2.length() || cfg.ssid3.length(); }

// ---------- on-device Wi-Fi setup: scan and connect on request from the UI ----------
WifiNet wifiNets[16];
volatile int wifiNetsN = 0;
volatile bool wifiScanBusy = false;
volatile int wifiJoin = 0;
String wifiJoinSsid, wifiJoinErr;
static volatile bool scanReq = false, joinReq = false;
static String joinPass;
static uint32_t joinAt = 0;
void netRequestScan() { if (!wifiScanBusy) { wifiScanBusy = true; scanReq = true; } }
void netConnectNow(const String& ssid, const String& pass) {
  lock(); wifiJoinSsid = ssid; joinPass = pass; wifiJoinErr = ""; unlock();
  wifiJoin = 1;
  joinReq = true;
}
static void doScan() {
  int n = WiFi.scanNetworks();
  lock();
  int k = 0;
  for (int i = 0; i < n; i++) {                       // strongest first, one entry per name
    int best = -1;
    for (int j = 0; j < n; j++) {
      String sj = WiFi.SSID(j);
      if (!sj.length()) continue;
      bool taken = false;
      for (int q = 0; q < k; q++) if (wifiNets[q].ssid == sj) { taken = true; break; }
      if (taken) continue;
      if (best < 0 || WiFi.RSSI(j) > WiFi.RSSI(best)) best = j;
    }
    if (best < 0 || k >= 16) break;
    wifiNets[k].ssid = WiFi.SSID(best); wifiNets[k].rssi = WiFi.RSSI(best);
    wifiNets[k].open = WiFi.encryptionType(best) == WIFI_AUTH_OPEN;
    k++;
  }
  wifiNetsN = k;
  unlock();
  WiFi.scanDelete();
  wifiScanBusy = false;
  logf("[net] scan: %d networks\n", k);
  uiRequestRedraw();
}

// ---------- setup portal ----------
static void scanNetworks() {
  int n = WiFi.scanNetworks();
  scanOptions = "";
  for (int i = 0; i < n && i < 15; i++) {
    String s = WiFi.SSID(i);
    if (!s.length() || scanOptions.indexOf("\"" + esc(s) + "\"") >= 0) continue;
    scanOptions += "<option value=\"" + esc(s) + "\">";
  }
  WiFi.scanDelete();
}

static void startAP() {
  WiFi.mode(WIFI_AP_STA);
  scanNetworks();
  WiFi.softAP(AP_SSID, cfg.apPass.c_str());
  dns.start(53, "*", WiFi.softAPIP());
  apMode = true;
  logf("[net] setup AP " AP_SSID " up at %s\n", WiFi.softAPIP().toString().c_str());
  ledSet(0, 0, 1);
  uiRequestRedraw();
}

static String field(const char* id, const char* label, const String& val, const char* type = "text", const char* ph = "") {
  return String("<label for=\"") + id + "\">" + label + "</label><input id=\"" + id + "\" name=\"" + id + "\" type=\"" + type +
         "\" value=\"" + esc(val) + "\" placeholder=\"" + ph + "\">";
}

static String secretField(const char* id, const char* label, bool isSet, const char* ph) {
  return String("<label for=\"") + id + "\">" + label + "</label><input id=\"" + id + "\" name=\"" + id +
         "\" type=\"password\" value=\"\" placeholder=\"" + (isSet ? "(salvo — deixe vazio pra manter)" : ph) + "\">";
}

static void handleRoot() {
  String h;
  h.reserve(9000);
  h += F("<!doctype html><html lang=pt-BR><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
         "<title>" PROJECT_NAME "</title><style>"
         "body{margin:0;background:#F2F4F1;color:#16191D;font:15px system-ui,sans-serif}"
         "header{background:#fff;border-bottom:3px solid #F7931A;padding:16px 20px}"
         "header b{font-size:20px}.tag{background:#F7931A;border-radius:4px;padding:2px 6px;font-weight:700;margin-right:8px}"
         "main{padding:12px 20px 40px;max-width:520px}h2{font-size:13px;letter-spacing:.08em;color:#A9530A;margin:22px 0 8px}"
         "label{display:block;font-size:13px;font-weight:600;margin:10px 0 4px}"
         "input,select{width:100%;box-sizing:border-box;height:42px;border:1px solid #C9D0CA;border-radius:8px;padding:0 12px;font:inherit;background:#fff}"
         "input:focus{outline:2px solid #F7931A}.row{display:grid;grid-template-columns:1fr 1fr;gap:10px}"
         "button{margin-top:24px;width:100%;height:48px;border:0;border-radius:10px;background:#16191D;color:#fff;font:600 16px system-ui}"
         "small{color:#5B6570;font-weight:400;letter-spacing:0}.mods{display:grid;grid-template-columns:1fr 1fr 1fr;gap:6px}"
         ".chk{display:flex;align-items:center;gap:6px;font-weight:500;margin:4px 0}.chk input{width:18px;height:18px}</style>"
         "<header><span class=tag>CD</span><b>" PROJECT_NAME "</b><br><small>Página servida pela própria placa · tudo fica salvo nela</small></header>"
         "<main><form method=post action=/save>");
  if (!apMode) h += field("pin", "Senha do painel (aparece na tela Config › Pelo celular)", "", "password", "8 dígitos");
  h += F("<h2>WI-FI <small>(até 3 redes · conecta na mais forte por perto)</small></h2>");
  {
    const char* sid[3] = {"ssid", "ssid2", "ssid3"};
    const char* pid[3] = {"wpass", "wpass2", "wpass3"};
    const String* sv[3] = {&cfg.ssid, &cfg.ssid2, &cfg.ssid3};
    const String* pv[3] = {&cfg.wpass, &cfg.wpass2, &cfg.wpass3};
    for (int k = 0; k < 3; k++) {
      h += String("<div class=row><div><label for=") + sid[k] + ">Rede " + (k + 1) + "</label><input id=" + sid[k] + " name=" + sid[k] +
           " list=nets value=\"" + esc(*sv[k]) + "\" placeholder=\"" + (k ? "opcional" : "nome da rede") + "\"></div><div>" +
           secretField(pid[k], "Senha", pv[k]->length() > 0, k ? "opcional" : "senha") + "</div></div>";
    }
    h += "<datalist id=nets>" + scanOptions + "</datalist>";
  }
  h += F("<h2>MÓDULOS <small>(desmarque o que não usa)</small></h2><div class=mods>");
  {
    const char* mn[M_COUNT] = {"Bitcoin", "Clima", "Seguidores", "Casa", "Agente", "Jogos", "Controle IR", "Mesh", "Pi-hole", "Agenda", "Notícias", "Máquinas"};
    for (int i = 0; i < M_COUNT; i++)
      h += String("<label class=chk><input type=checkbox name=m") + i + ((cfg.mods >> i) & 1 ? " checked" : "") + "> " + mn[i] + "</label>";
    h += "<input type=hidden name=mods_form value=1></div>";
  }
  h += F("<h2>FEED DO SEU SERVIDOR <small>(agenda, manchetes, máquinas, uso de IA)</small></h2>");
  h += field("nome", "Seu nome (saudação na tela)", cfg.nome, "text", "opcional");
  h += field("feed", "URL do feed (JSON, veja docs/feed.md)", cfg.feedUrl, "url", "https://seu-servidor/feed.json");
  h += secretField("feedtok", "Token do feed (vai como Authorization: Bearer)", cfg.feedToken.length() > 0, "opcional");
  h += String("<label class=chk><input type=checkbox name=night") + (cfg.nightDim ? " checked" : "") + "> Modo noite (brilho baixo das 23h às 7h)</label>";
  h += String("<label class=chk><input type=checkbox name=abeep") + (cfg.alertBeep ? " checked" : "") + "> Bipe nos avisos (nunca de madrugada)</label>";
  h += F("<input type=hidden name=opts_form value=1>");
  h += "<h2>FIRMWARE</h2><p>Esta placa: <b>" FW_VERSION "</b>";
  if (fwNewer()) h += " · nova versão <b>" + esc(live.fwLatest) + "</b> no GitHub. Marque abaixo e salve com o PIN para instalar.</p><label class=chk><input type=checkbox name=fwinst> Instalar a versão " + esc(live.fwLatest) + " agora</label>";
  else h += live.fwLatest.length() ? " · é a mais nova</p>" : " · ainda não consultei o GitHub</p>";
  h += F("<h2>AGENDA <small>(link iCal: no Google Agenda, Configurações do calendário › Integrar agenda › Endereço secreto no formato iCal)</small></h2>");
  h += secretField("ical", "Link .ics da agenda (substitui a agenda do feed)", cfg.icalUrl.length() > 0, "https://calendar.google.com/calendar/ical/.../basic.ics");
  if (cfg.icalUrl.length()) h += F("<label class=chk><input type=checkbox name=icalclr> Remover o link da agenda</label>");
  h += F("<h2>SEGUIDORES</h2>");
  h += field("n8n", "URL dos seguidores (JSON: total, instagram, youtube…)", cfg.n8nUrl, "url", "https://seu-servidor/seguidores.json");
  h += F("<h2>PI-HOLE</h2>");
  h += "<div class=row><div>" + field("phurl", "Endereço do Pi-hole", cfg.phUrl, "url", "http://192.168.1.50") + "</div><div>" +
       secretField("phpass", "Senha (ou senha de app)", cfg.phPass.length() > 0, "senha") + "</div></div>";
  h += F("<h2>CASA · HOME ASSISTANT</h2>");
  h += field("haurl", "Endereço do Home Assistant", cfg.haUrl, "url", "http://192.168.0.10:8123");
  h += secretField("hatok", "Token de longa duração", cfg.haToken.length() > 0, "token");
  for (int i = 0; i < 4; i++) {
    char a[8], b[8], la[24], lb[24];
    snprintf(a, sizeof(a), "han%d", i); snprintf(b, sizeof(b), "hae%d", i);
    snprintf(la, sizeof(la), "Botão %d: nome", i + 1); snprintf(lb, sizeof(lb), "Botão %d: entidade", i + 1);
    h += "<div class=row><div>" + field(a, la, cfg.haName[i]) + "</div><div>" + field(b, lb, cfg.haEnt[i], "text", "light.sala") + "</div></div>";
  }
  h += F("<h2>AGENTE · HERMES GADGET</h2>");
  h += field("hermes", "Endereço do gadget no Hermes (hermes gadget info)", cfg.hermesUrl, "text", "ws://192.168.0.20:8765/gadget");
  h += secretField("htok", "Token de acesso (só se o Hermes usar GADGET_ACCESS_TOKEN)", cfg.hermesToken.length() > 0, "opcional");
  h += F("<h2>MESH · MESHTASTIC</h2>");
  h += field("mesh", "IP do nó base", cfg.meshIp, "text", "192.168.0.30");
  h += F("<h2>PAINEL</h2>");
  h += "<div class=row><div>" + field("city", "Cidade do clima (vazio = pelo IP)", cfg.city) + "</div><div><label for=btcev>Atualizar BTC</label><select id=btcev name=btcev>";
  int opts[3] = {30, 60, 300};
  const char* names[3] = {"30 s", "60 s", "5 min"};
  for (int i = 0; i < 3; i++) h += String("<option value=") + opts[i] + (cfg.btcEvery == opts[i] ? " selected>" : ">") + names[i] + "</option>";
  h += F("</select></div></div><label for=theme>Tema</label><select id=theme name=theme>");
  { const char* tn[3] = {"Claro", "Terminal (escuro)", "Terminal claro"};
    for (int i = 0; i < 3; i++) h += String("<option value=") + i + (cfg.theme == i ? " selected>" : ">") + tn[i] + "</option>"; }
  h += String("</select><label class=chk><input type=checkbox name=anim") + (cfg.anim ? " checked" : "") + "> Efeitos animados no tema Terminal</label>";
  h += String("<label class=chk><input type=checkbox name=apagao") + (cfg.apagao ? " checked" : "") + "> Modo apagão: troca de tela quando a internet cai</label>";
  h += F("<button type=submit>Salvar na placa</button></form>"
         "<p><small>" PROJECT_NAME " · firmware " FW_VERSION " · os campos de senha nunca são mostrados de volta · <a href=/update>atualizar firmware</a></small></p></main></html>");
  server.send(200, "text/html; charset=utf-8", h);
}

static void handleSave() {
  if (!apMode && server.arg("pin") != cfg.apPass) {
    server.send(403, "text/html; charset=utf-8", "<meta charset=utf-8><p style='font:16px system-ui'>Senha do painel errada. Veja em Config › Pelo celular.</p>");
    return;
  }
  auto take = [](const char* k, String& dst) { if (server.hasArg(k)) { dst = server.arg(k); dst.trim(); } };
  auto takeSecret = [](const char* k, String& dst) { if (server.hasArg(k) && server.arg(k).length()) dst = server.arg(k); };
  String oldCity = cfg.city;
  take("ssid", cfg.ssid); takeSecret("wpass", cfg.wpass);
  take("ssid2", cfg.ssid2); takeSecret("wpass2", cfg.wpass2);
  take("ssid3", cfg.ssid3); takeSecret("wpass3", cfg.wpass3);
  take("n8n", cfg.n8nUrl);
  take("phurl", cfg.phUrl); takeSecret("phpass", cfg.phPass);
  if (server.hasArg("fwinst")) netRequestFwInstall();
  take("nome", cfg.nome); take("feed", cfg.feedUrl); takeSecret("feedtok", cfg.feedToken);
  takeSecret("ical", cfg.icalUrl); if (server.hasArg("icalclr")) cfg.icalUrl = "";
  if (server.hasArg("opts_form")) { cfg.nightDim = server.hasArg("night"); cfg.alertBeep = server.hasArg("abeep"); }
  if (server.hasArg("mods_form")) {
    uint16_t m = 0;
    for (int i = 0; i < M_COUNT; i++) if (server.hasArg(String("m") + i)) m |= (1 << i);
    cfg.mods = m;
  }
  take("haurl", cfg.haUrl); takeSecret("hatok", cfg.haToken);
  for (int i = 0; i < 4; i++) {
    char a[8], b[8];
    snprintf(a, sizeof(a), "han%d", i); snprintf(b, sizeof(b), "hae%d", i);
    take(a, cfg.haName[i]); take(b, cfg.haEnt[i]);
  }
  take("hermes", cfg.hermesUrl); takeSecret("htok", cfg.hermesToken);
  take("mesh", cfg.meshIp); take("city", cfg.city);
  if (server.hasArg("btcev")) cfg.btcEvery = constrain(server.arg("btcev").toInt(), 30, 600);
  if (server.hasArg("theme")) cfg.theme = constrain(server.arg("theme").toInt(), 0, 2);
  if (server.hasArg("opts_form")) { cfg.anim = server.hasArg("anim"); cfg.apagao = server.hasArg("apagao"); }
  if (cfg.city != oldCity) { cfg.lat = NAN; cfg.lon = NAN; ipLat = NAN; }
  configSave();
  server.send(200, "text/html; charset=utf-8",
              "<meta charset=utf-8><meta name=viewport content='width=device-width'><p style='font:18px system-ui;padding:20px'>"
              "<b>Salvo.</b> A placa vai reiniciar e conectar na sua rede.</p>");
  delay(1200);
  ESP.restart();
}

static void handleStatus() {
  JsonDocument d;
  d["fw"] = FW_VERSION; d["ip"] = ipStr; d["wifi"] = (bool)wifiUp; d["ap"] = (bool)apMode;
  d["heap"] = ESP.getFreeHeap(); d["maxblk"] = ESP.getMaxAllocHeap(); d["uptime_s"] = millis() / 1000;
  d["net_beat_s"] = (millis() - netHeartbeat) / 1000; d["online"] = (bool)netOnline; d["ssh"] = sshState;
  d["btc"] = live.btc; d["wx"] = live.wxOk; d["feed"] = live.plOk; d["agenda"] = live.agN; d["hermes"] = hermes.status;
  d["erro_feed"] = live.plErr; d["erro_agenda"] = live.icErr;
  { time_t t; time(&t); d["relogio_ok"] = t > 1700000000; d["epoch"] = (long)t; }
  d["versao_github"] = live.fwLatest;
  String s; serializeJson(d, s);
  server.send(200, "application/json", s);
}

// ---------- firmware update over Wi-Fi (POST /update, header X-Pin, multipart field "fw") ----------
// The PIN goes in a header, never in the URL, so it does not end up in the browser history.
static bool otaAuth = false, otaOk = false;
static void handleUpdatePage() {
  String h = F("<!doctype html><html lang=pt-BR><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
               "<title>Atualizar · " PROJECT_NAME "</title><style>body{margin:0;background:#F2F4F1;color:#16191D;font:15px system-ui,sans-serif}"
               "main{padding:20px;max-width:480px}input{display:block;width:100%;box-sizing:border-box;margin:8px 0 14px;font:inherit}"
               "input[type=password]{height:42px;border:1px solid #C9D0CA;border-radius:8px;padding:0 12px}"
               "button{width:100%;height:48px;border:0;border-radius:10px;background:#16191D;color:#fff;font:600 16px system-ui}"
               "small{color:#5B6570}</style><main><h2>Atualizar firmware</h2>"
               "<p><small>Versão atual: " FW_VERSION ". Use o arquivo <b>-ota.bin</b> (não o merged).</small></p>"
               "<form id=f onsubmit=\"event.preventDefault();var fd=new FormData();fd.append('fw',this.fw.files[0]);this.b.disabled=true;this.b.textContent='Enviando… não desligue';"
               "fetch('/update',{method:'POST',headers:{'X-Pin':this.pin.value},body:fd}).then(function(r){return r.text()}).then(function(t){document.body.innerHTML=t})"
               ".catch(function(){document.body.innerHTML='<p style=padding:20px>Falhou o envio. Recarregue e tente de novo.</p>'});\">"
               "<label>Senha do painel</label><input name=pin type=password inputmode=numeric>"
               "<label>Arquivo .bin</label><input name=fw type=file accept=.bin>"
               "<button name=b>Enviar e reiniciar</button></form></main></html>");
  server.send(200, "text/html; charset=utf-8", h);
}
static void handleUpdateUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    otaAuth = apMode || server.header("X-Pin") == cfg.apPass;
    otaOk = false;
    if (!otaAuth) { Serial.println("[ota] senha errada"); return; }
    Serial.printf("[ota] recebendo %s\n", up.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (otaAuth && Update.isRunning() && Update.write(up.buf, up.currentSize) != up.currentSize) Update.printError(Serial);
  } else if (up.status == UPLOAD_FILE_END) {
    if (otaAuth && Update.isRunning()) { otaOk = Update.end(true); Serial.printf("[ota] %u bytes, ok=%d\n", up.totalSize, otaOk); if (!otaOk) Update.printError(Serial); }
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (Update.isRunning()) Update.abort();
  }
}
static void handleUpdateDone() {
  String msg = !otaAuth ? "Senha do painel errada." : otaOk ? "Firmware gravado. Reiniciando…" : String("Falhou: ") + Update.errorString();
  server.send(otaOk ? 200 : (otaAuth ? 500 : 403), "text/html; charset=utf-8",
              "<meta charset=utf-8><meta name=viewport content='width=device-width'><p style='font:18px system-ui;padding:20px'>" + msg + "</p>");
  if (otaOk) { delay(800); ESP.restart(); }
}

static void portalBegin() {
  static const char* hdrs[] = {"X-Pin"};
  server.collectHeaders(hdrs, 1);
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleUpdateDone, handleUpdateUpload);
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/log", HTTP_GET, []() { server.send(200, "text/plain; charset=utf-8", logDump()); });
  server.on("/demo", HTTP_GET, []() {
    if (server.arg("on") == "0") { server.send(200, "text/plain", "saindo do modo demonstração (reinicia)"); delay(300); ESP.restart(); }
    demoMode = true; demoSince = millis(); demoFill(); uiRequestRedraw();
    server.send(200, "text/plain; charset=utf-8", "modo demonstração ligado por 15 min: dados de exemplo na tela, /shot.bmp?tela=home liberado");
  });
  server.on("/shot.bmp", HTTP_GET, []() {
    // only with sample data on the screen: a real screen can show agenda, machines, followers
    if (!demoMode) { server.send(403, "text/plain; charset=utf-8", "ligue antes o modo demonstração: /demo?on=1"); return; }
    if (server.hasArg("tela")) { int sc = screenFromName(server.arg("tela")); if (sc >= 0) uiGoto(sc); }
    delay(400);
    server.setContentLength(UI_BMP_SIZE);
    server.send(200, "image/bmp", "");
    WiFiClient c = server.client();
    uiScreenshotBmp(c);
  });
  server.onNotFound([]() {
    if (apMode) { server.sendHeader("Location", "http://192.168.4.1/"); server.send(302, "text/plain", ""); }
    else server.send(404, "text/plain", "not found");
  });
  server.begin();
}

void portalLoop() {
  if (apMode) dns.processNextRequest();
  server.handleClient();
}

// ---------- internet check (blackout mode) ----------
// A plain TCP connect to two public resolvers: no data sent, nothing to log on their side but a SYN.
volatile bool netOnline = true;
volatile uint32_t offlineSince = 0;
static bool reachInternet() {
  WiFiClient c;
  if (c.connect(IPAddress(1, 1, 1, 1), 443, 3000)) { c.stop(); return true; }
  if (c.connect(IPAddress(9, 9, 9, 9), 443, 3000)) { c.stop(); return true; }
  return false;
}
static void setOnline(bool on) {
  if (on == netOnline) return;
  if (on) {
    uint32_t mins = offlineSince ? (millis() - offlineSince) / 60000 : 0;
    netOnline = true; offlineSince = 0;
    logf("[net] internet back after %u min\n", mins);
    notifyAdd(N_REDE, "Internet voltou", mins ? "ficou fora " + String(mins) + " min" : String("conexão restabelecida"));
  } else {
    netOnline = false; offlineSince = millis();
    logf("[net] internet down");
    notifyAdd(N_REDE, "Internet caiu", wifiUp ? "Wi-Fi conectado, sem saída para a internet" : "sem Wi-Fi");
  }
  uiRequestRedraw();
}

// ---------- radio: HF propagation + next ISS pass ----------
static volatile bool radioReq = false;
void netRequestRadio() { radioReq = true; }
static String xmlTag(const String& b, const char* tag) {
  String open = String("<") + tag + ">", close = String("</") + tag + ">";
  int a = b.indexOf(open); if (a < 0) return "";
  a += open.length();
  int e = b.indexOf(close, a); if (e < 0) return "";
  String v = b.substring(a, e); v.trim();
  return v;
}
static int numOr(const String& v) { return v.length() && isDigit(v[0]) ? v.toInt() : -1; }
static void fetchPropagation() {
  // N0NBH (hamqsl.com) allows free use with credit; the data refreshes every few hours
  String b;
  bool ok = httpGet("https://www.hamqsl.com/solarxml.php", b, "", nullptr, "", "application/xml");
  if (!ok) ok = httpGet("http://www.hamqsl.com/solarxml.php", b, "", nullptr, "", "application/xml");
  if (!ok || b.indexOf("<solardata>") < 0) { lock(); live.rdErr = "propagação sem resposta"; unlock(); return; }
  static const char* BANDS[4] = {"80m-40m", "30m-20m", "17m-15m", "12m-10m"};
  uint8_t cond[2][4] = {{0}};
  for (int t = 0; t < 2; t++) for (int i = 0; i < 4; i++) {
    String key = String("<band name=\"") + BANDS[i] + "\" time=\"" + (t ? "night" : "day") + "\">";
    int a = b.indexOf(key); if (a < 0) continue;
    String v = b.substring(a + key.length(), b.indexOf('<', a + key.length()));
    cond[t][i] = v == "Good" ? 3 : v == "Fair" ? 2 : v == "Poor" ? 1 : 0;
  }
  lock();
  live.rdSfi = numOr(xmlTag(b, "solarflux")); live.rdA = numOr(xmlTag(b, "aindex"));
  live.rdK = numOr(xmlTag(b, "kindex")); live.rdSpots = numOr(xmlTag(b, "sunspots"));
  live.rdXray = xmlTag(b, "xray"); live.rdGeo = xmlTag(b, "geomagfield"); live.rdNoise = xmlTag(b, "signalnoise");
  memcpy(live.rdBand, cond, sizeof(cond));
  time_t now; time(&now); live.rdAt = now; live.rdOk = true; live.rdErr = "";
  unlock();
  logf("[net] radio sfi %d k %d a %d\n", live.rdSfi, live.rdK, live.rdA);
}

static char tle1[80], tle2[80];
static uint32_t tleAt = 0;          // millis() of the last TLE download
static double jd2unix(double jd) { return (jd - 2440587.5) * 86400.0; }
static void fetchIssPass() {
  float lat = cfg.lat, lon = cfg.lon;
  if (isnan(lat)) { if (isnan(ipLat) && !ipLocate()) { lock(); live.rdErr = "sem localização para a ISS"; unlock(); return; } lat = ipLat; lon = ipLon; }
  if (!tle1[0] || millis() - tleAt > 12UL * 3600 * 1000) {     // CelesTrak asks for at most one download every few hours
    String b;
    if (httpGet("https://celestrak.org/NORAD/elements/gp.php?CATNR=25544&FORMAT=TLE", b, "", nullptr, "", "text/plain")) {
      int l1 = b.indexOf("\n1 "), l2 = b.indexOf("\n2 ");
      if (l1 > 0 && l2 > l1) {
        String a = b.substring(l1 + 1, b.indexOf('\n', l1 + 1)), c = b.substring(l2 + 1, b.indexOf('\n', l2 + 1));
        a.trim(); c.trim();
        strlcpy(tle1, a.c_str(), sizeof(tle1)); strlcpy(tle2, c.c_str(), sizeof(tle2)); tleAt = millis();
      }
    }
    if (!tle1[0]) { lock(); live.rdErr = "órbita da ISS sem resposta"; unlock(); return; }
  }
  time_t now; time(&now);
  static Sgp4 sat;                    // ~1 KB of orbit state: keep it off the task stack
  sat.site(lat, lon, 100);
  sat.init("ISS", tle1, tle2);
  sat.initpredpoint((unsigned long)now, 0.0);
  passinfo p;
  bool found = false;
  for (int k = 0; k < 6 && !found; k++) {
    if (!sat.nextpass(&p, 20, false, 10.0)) break;   // passes at least 10° above the horizon
    if (jd2unix(p.jdstop) > now) found = true;
  }
  lock();
  live.issOk = found;
  if (found) {
    live.issAos = (uint32_t)jd2unix(p.jdstart); live.issLos = (uint32_t)jd2unix(p.jdstop);
    live.issMaxEl = (int)lround(p.maxelevation); live.issAzAos = (int)lround(p.azstart); live.issAzLos = (int)lround(p.azstop);
  }
  unlock();
  logf("[net] iss next pass %s el %d\n", found ? "ok" : "none", live.issMaxEl);
}

static void netHeartbeatTouch();
static bool timeIsSet() { time_t t; time(&t); return t > 1700000000; }
// ---------- agenda from an iCal (.ics) link ----------
// The calendar file can hold years of events: it is parsed while it downloads, nothing is kept but the next ones.
#include "ical.h"
class IcsSink : public Stream {   // HTTPClient::writeToStream wants a Stream; only write() is used
 public:
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  IcsAgenda* a; size_t total = 0; uint32_t t0 = 0; bool tooBig = false;
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* b, size_t n) override {
    if (total > 4UL * 1024 * 1024 || millis() - t0 > 60000) { tooBig = true; return 0; }   // stop the download
    a->feed((const char*)b, n); total += n;
    if ((total & 0xFFFF) < n) netHeartbeatTouch();
    return n;
  }
};
static void fetchIcal() {
  time_t now; time(&now);
  if (now < 1700000000) return;                 // needs the right time to know what is "next"
  IcsAgenda* agp = new IcsAgenda;               // ~4 KB of state, only while reading
  IcsAgenda& ag = *agp;
  ag.begin(now, now + 14L * 86400, -3 * 3600);
  HTTPClient http;
  WiFiClientSecure sec;
  WiFiClient plain;
  bool ok = cfg.icalUrl.startsWith("https") ? (sec.setCACert(ROOT_CAS), http.begin(sec, cfg.icalUrl)) : http.begin(plain, cfg.icalUrl);
  if (!ok) { delete agp; lock(); live.icErr = "link da agenda inválido"; unlock(); return; }
  http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  int code = http.GET();
  IcsSink sink; sink.a = &ag; sink.t0 = millis();
  int got = code == 200 ? http.writeToStream(&sink) : -1;
  http.end();
  if (code != 200 || (got < 0 && !sink.tooBig)) {
    logf("[net] ical -> %d (%d)\n", code, got);
    lock(); live.icErr = code == 404 || code == 401 || code == 403 ? "link da agenda recusado (troque o link)" : "agenda sem resposta"; unlock();
    delete agp;
    return;
  }
  IcsEvent ev[3];
  int n = ag.finish(ev, 3);
  lock();
  live.agN = n;
  for (int i = 0; i < n; i++) { live.agT[i] = ev[i].title; live.agQ[i] = (uint32_t)ev[i].start; live.agDia[i] = ev[i].allDay; }
  live.icOk = true; live.icAt = now; live.icErr = sink.tooBig ? "agenda muito grande: só parte foi lida" : "";
  unlock();
  long evs = ag.events;
  delete agp;
  logf("[net] ical %u KB, %ld eventos, %d próximos\n", (unsigned)(sink.total / 1024), evs, n);
}


// ---------- updates from the GitHub releases ----------
// The board asks the GitHub API for the latest release of UPDATE_REPO, downloads its
// cybercydeck-<version>-ota.bin over HTTPS, checks it against the release's SHA256SUMS.txt while
// writing, and only then switches to it. Nothing installs by itself: you tap to install.
#include <Update.h>
#include <mbedtls/sha256.h>
static volatile bool fwCheckReq = false, fwInstallReq = false;
void netRequestFwCheck() { fwCheckReq = true; }
void netRequestFwInstall() { fwInstallReq = true; }
static long verNum(String v) {             // "v0.3.10" -> 3010 (major, minor, patch)
  v.trim(); if (v.startsWith("v")) v = v.substring(1);
  int d1 = v.indexOf('.'), d2 = d1 > 0 ? v.indexOf('.', d1 + 1) : -1;
  long a = v.toInt(), b = d1 > 0 ? v.substring(d1 + 1).toInt() : 0, c = d2 > 0 ? v.substring(d2 + 1).toInt() : 0;
  return a * 1000000L + b * 1000L + c;
}
bool fwNewer() { return live.fwLatest.length() && verNum(live.fwLatest) > verNum(FW_VERSION) && live.fwUrl.length(); }
static void fwCheck() {
  String b;
  if (!httpGet("https://api.github.com/repos/" UPDATE_REPO "/releases/latest", b, "", nullptr, "", "application/vnd.github+json")) {
    lock(); live.fwErr = lastHttp == 404 ? "nenhuma versão publicada ainda" : "não consegui consultar o GitHub"; unlock(); return;
  }
  JsonDocument filter;
  filter["tag_name"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;
  JsonDocument d;
  if (deserializeJson(d, b, DeserializationOption::Filter(filter))) { lock(); live.fwErr = "resposta do GitHub inválida"; unlock(); return; }
  String tag = d["tag_name"] | "", url, sums;
  for (JsonObject a : d["assets"].as<JsonArray>()) {
    String n = a["name"] | "";
    if (n.endsWith("-ota.bin")) url = a["browser_download_url"] | "";
    else if (n == "SHA256SUMS.txt") sums = a["browser_download_url"] | "";
  }
  String sha;
  if (url.length() && sums.length() && httpGet(sums, b, "", nullptr, "", "text/plain")) {
    String name = url.substring(url.lastIndexOf('/') + 1);
    int i = b.indexOf("  " + name);                             // "<sha256>  <file>"
    if (i >= 64) sha = b.substring(i - 64, i);
  }
  bool wasNewer = fwNewer();
  time_t now; time(&now);
  lock();
  live.fwLatest = tag.startsWith("v") ? tag.substring(1) : tag; live.fwUrl = sha.length() == 64 ? url : ""; live.fwSha = sha; live.fwAt = now;
  live.fwErr = url.length() && sha.length() != 64 ? "release sem SHA256SUMS: não instalo" : "";
  unlock();
  logf("[net] última versão no GitHub: %s (esta: %s)", tag.c_str(), FW_VERSION);
  if (fwNewer() && !wasNewer) notifyAdd(N_AGENTE, "Versão " + live.fwLatest + " disponível", "Config › toque na versão para instalar");
}
static void fwInstall() {
  if (!fwNewer()) return;
  String url = live.fwUrl, want = live.fwSha; want.toLowerCase();
  auto fail = [](const String& m) { logf("[net] atualização falhou: %s", m.c_str()); lock(); live.fwErr = m; live.fwProg = -1; unlock(); uiRequestRedraw(); };
  lock(); live.fwProg = 0; live.fwErr = ""; unlock(); uiRequestRedraw();
  HTTPClient http; WiFiClientSecure sec;
  sec.setCACert(ROOT_CAS);
  if (!http.begin(sec, url)) { fail("endereço inválido"); return; }
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  int code = http.GET(), len = http.getSize();
  if (code != 200 || len <= 0) { http.end(); fail("download recusado (" + String(code) + ")"); return; }
  if (!Update.begin(len)) { http.end(); fail(String("sem espaço: ") + Update.errorString()); return; }
  mbedtls_sha256_context ctx; mbedtls_sha256_init(&ctx); mbedtls_sha256_starts(&ctx, 0);
  WiFiClient* st = http.getStreamPtr();
  static uint8_t buf[2048];
  int got = 0, lastPct = -1; uint32_t t0 = millis(), lastByte = millis();
  while (got < len && millis() - t0 < 180000 && millis() - lastByte < 20000) {
    int av = st->available();
    if (av <= 0) { vTaskDelay(pdMS_TO_TICKS(5)); continue; }
    int n = st->readBytes(buf, min(av, (int)sizeof(buf)));
    if (n <= 0) continue;
    mbedtls_sha256_update(&ctx, buf, n);
    if (Update.write(buf, n) != (size_t)n) break;
    got += n; lastByte = millis(); netHeartbeat = millis();
    int pct = (long)got * 100 / len;
    if (pct != lastPct) { lastPct = pct; lock(); live.fwProg = pct; unlock(); if (pct % 5 == 0) uiRequestRedraw(); }
  }
  http.end();
  uint8_t h[32]; mbedtls_sha256_finish(&ctx, h); mbedtls_sha256_free(&ctx);
  char hex[65]; for (int i = 0; i < 32; i++) sprintf(hex + i * 2, "%02x", h[i]);
  if (got != len) { Update.abort(); fail("download interrompido"); return; }
  if (want != hex) { Update.abort(); fail("arquivo não confere com o SHA-256 da release"); return; }
  if (!Update.end(true)) { fail(String("não gravou: ") + Update.errorString()); return; }
  logf("[net] versão %s instalada, reiniciando", live.fwLatest.c_str());
  lock(); live.fwProg = 100; unlock(); uiRequestRedraw();
  delay(1500);
  ESP.restart();
}

// ---------- demo mode: sample data for screenshots (README, docs), nothing personal ----------
// Turned on over the LAN at /demo?on=1; the fetches stop, and /shot.bmp?tela=<screen> works.
// It turns itself off (restart) after 15 minutes, or at /demo?on=0.
volatile bool demoMode = false;
static void demoFill() {
  time_t now; time(&now);
  lock();
  live.btcOk = true; live.btc = 97450; live.btcChg = 2.31f; live.eth = 3612; live.ethChg = -0.84f; live.sol = 238; live.solChg = 4.12f;
  live.feeSlow = 3; live.feeStd = 5; live.feeFast = 8; live.fng = 64; live.fngLabel = "Greed"; live.btcAt = now;
  live.wxOk = true; live.wxTemp = 24; live.wxHum = 61; live.wxWind = 9; live.wxCode = 2; live.wxPlace = "São Paulo"; live.wxByIp = false; live.wxAt = now;
  const float mx[4] = {27, 29, 25, 23}, mn[4] = {18, 19, 17, 16}; const int cd[4] = {2, 1, 61, 3};
  for (int i = 0; i < 4; i++) { live.dMax[i] = mx[i]; live.dMin[i] = mn[i]; live.dCode[i] = cd[i]; struct tm t; time_t d = now + 86400L * (i + 1); localtime_r(&d, &t); const char* W[7] = {"dom.", "seg.", "ter.", "qua.", "qui.", "sex.", "sáb."}; live.dDay[i] = W[t.tm_wday]; }
  live.socOk = true; live.socYt = 48200; live.socIg = 312500; live.socTt = 157800; live.socX = 22400; live.socTg = 9100; live.socTh = 41000; live.socFb = 18600;
  live.socTotal = 48200 + 312500 + 157800 + 22400 + 9100 + 41000 + 18600; live.socAt = now;
  live.phOk = true; live.phTotal = 48213; live.phBlocked = 9870; live.phDomains = 312450; live.phPct = 20.5f; live.phClients = 14; live.phAt = now; live.phErr = "";
  live.plOk = true; live.plAt = now; live.plErr = ""; live.saude = "ok";
  live.mqN = 3;
  const char* nm[3] = {"Servidor", "Raspberry Pi", "NAS"}; const int cpu[3] = {23, 8, 4}, ram[3] = {61, 37, 52}, tp[3] = {54, 47, 41}, dk[3] = {68, 41, 77};
  for (int i = 0; i < 3; i++) { live.mqNome[i] = nm[i]; live.mqCpu[i] = cpu[i]; live.mqRam[i] = ram[i]; live.mqTemp[i] = tp[i]; live.mqDisk[i] = dk[i]; live.mqAge[i] = 40; live.mqOn[i] = true; }
  live.iaCusto = 37.80f; live.iaTokens = 41.2e6; live.iaN = 2; live.iaNome[0] = "Servidor"; live.iaC[0] = 29.10f; live.iaNome[1] = "Raspberry Pi"; live.iaC[1] = 8.70f;
  live.agN = 3; live.icOk = true; live.icAt = now; live.icErr = "";
  live.agT[0] = "Reunião de equipe"; live.agQ[0] = now + 50 * 60; live.agDia[0] = false;
  live.agT[1] = "Dentista"; live.agQ[1] = now + 26 * 3600; live.agDia[1] = false;
  live.agT[2] = "Aniversário da Ana"; live.agQ[2] = now + 3 * 86400; live.agDia[2] = true;
  live.nwN = 3; live.nwT[0] = "Nova versão do firmware disponível"; live.nwF[0] = "GitHub"; live.nwT[1] = "Rede mesh cobre o bairro todo"; live.nwF[1] = "Blog";
  live.nwT[2] = "Como montar seu próprio cyberdeck"; live.nwF[2] = "Fórum";
  live.rdOk = true; live.rdAt = now; live.rdSfi = 162; live.rdK = 2; live.rdA = 8; live.rdSpots = 118;
  const uint8_t bd[2][4] = {{2, 3, 3, 2}, {3, 2, 1, 1}}; memcpy(live.rdBand, bd, sizeof(bd));
  live.issOk = true; live.issAos = now + 47 * 60; live.issLos = now + 53 * 60; live.issMaxEl = 58; live.issAzAos = 225; live.issAzLos = 45; live.rdErr = "";
  unlock();
  cfg.nome = "Ana";   // only in RAM: the restart at the end brings the real one back
}


// ---------- clock fallback ----------
// HTTPS needs the right date to check certificates. If NTP does not answer (some networks block
// it), take the date from the "Date" header of a plain HTTP page. Only used while the clock is unset.
static uint32_t wifiUpAt = 0, lastDateTry = 0;
static bool clockSet() { time_t t; time(&t); return t > 1700000000; }
static void clockFromHttp() {
  HTTPClient http; WiFiClient c;
  const char* keys[] = {"Date"};
  if (!http.begin(c, "http://detectportal.firefox.com/success.txt")) return;
  http.collectHeaders(keys, 1);
  http.setTimeout(6000);
  int code = http.GET();
  String d = http.header("Date");
  http.end();
  // "Thu, 26 Nov 2026 07:10:00 GMT"
  static const char* M = "JanFebMarAprMayJunJulAugSepOctNovDec";
  int day, year, hh, mm, ss; char mon[4];
  if (code <= 0 || sscanf(d.c_str(), "%*3s, %d %3s %d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) != 6) {
    logf("[net] relógio: sem NTP e sem data por HTTP (%d)", code); return;
  }
  const char* f = strstr(M, mon); if (!f) return;
  struct tm t = {}; t.tm_year = year - 1900; t.tm_mon = (f - M) / 3; t.tm_mday = day; t.tm_hour = hh; t.tm_min = mm; t.tm_sec = ss;
  setenv("TZ", "UTC0", 1); tzset();
  time_t epoch = mktime(&t);
  setenv("TZ", "<-03>3", 1); tzset();
  struct timeval tv = {epoch, 0};
  settimeofday(&tv, nullptr);
  logf("[net] relógio acertado pela data HTTP (NTP não respondeu)");
}
// ---------- network task ----------
volatile uint32_t netHeartbeat = 0;   // read by the UI task: software watchdog for this task
static void netHeartbeatTouch() { netHeartbeat = millis(); }
static void netTask(void*) {
  uint32_t lastBtc = 0, lastWx = 0, lastSoc = 0, lastHa = 0, lastPh = 0, lastPl = 0, lastRd = 0, lastIc = 0, lastFw = 0, lastNetCheck = 0, wifiStart = millis();
  bool hermesStarted = false;
  for (;;) {
    netHeartbeat = millis();
    if (scanReq) { scanReq = false; doScan(); }
    if (joinReq) {
      joinReq = false;
      logf("[net] connecting to %s (asked on screen)\n", wifiJoinSsid.c_str());
      WiFi.disconnect();
      vTaskDelay(pdMS_TO_TICKS(150));
      WiFi.begin(wifiJoinSsid.c_str(), joinPass.c_str());
      joinAt = lastJoin = millis();
    }
    if (wifiJoin == 1) {
      if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == wifiJoinSsid) { wifiJoin = 2; uiRequestRedraw(); }
      else if (millis() - joinAt > 20000) {
        wl_status_t st = WiFi.status();
        lock(); wifiJoinErr = st == WL_NO_SSID_AVAIL ? "rede não encontrada" : "não conectou: senha errada ou sinal fraco"; unlock();
        wifiJoin = 3;
        logf("[net] join failed, status %d\n", (int)st);
        uiRequestRedraw();
      }
    }
    bool up = WiFi.status() == WL_CONNECTED;
    if (up && !wifiUp) {
      wifiUp = true; everUp = true;
      ipStr = WiFi.localIP().toString();
      logf("[net] wifi up %s rssi %d\n", ipStr.c_str(), WiFi.RSSI());
      // the router first (many answer NTP and some networks block outside NTP), then public servers
      static String gw; gw = WiFi.gatewayIP().toString();
      configTzTime("<-03>3", gw.c_str(), "pool.ntp.org", "time.cloudflare.com");
      wifiUpAt = millis();
      lastBtc = lastWx = lastSoc = lastHa = lastPh = lastPl = 0;
      if (apMode) { dns.stop(); WiFi.softAPdisconnect(true); WiFi.mode(WIFI_STA); apMode = false; }
      ledSet(0, 0, 0);
      uiRequestRedraw();
    } else if (!up && wifiUp) {
      wifiUp = false;
      logf("[net] wifi down");
      uiRequestRedraw();
    }
    if (!up && !apMode && !everUp && (!anySsid() || millis() - wifiStart > 25000)) startAP();
    // lost the network (or moved to another place): look for the strongest saved one again
    if (!up && anySsid() && wifiJoin != 1 && millis() - lastJoin > (apMode ? 120000UL : 20000UL)) joinBest();

    // internet check: every minute while online, every 20 s while offline; Wi-Fi gone counts as offline
    if (everUp && millis() - lastNetCheck > (netOnline ? 60000UL : 20000UL)) {
      lastNetCheck = millis();
      setOnline(wifiUp && reachInternet());
    }
    if (demoMode && millis() - demoSince > 15UL * 60 * 1000) ESP.restart();
    if (wifiUp && !clockSet() && millis() - wifiUpAt > 12000 && millis() - lastDateTry > 30000) { lastDateTry = millis(); clockFromHttp(); }
    if (wifiUp && (demoMode || sshState == 1 || sshState == 2)) { if (hermesStarted) hermesLoop(); }   // demo or terminal: no HTTPS fetches
    else if (wifiUp && clockSet()) {
      uint32_t now = millis();
      if (refreshReq) { refreshReq = false; lastBtc = lastWx = lastSoc = lastHa = lastPh = lastPl = lastIc = 0; }
      if (placaReq) { placaReq = false; if (now - lastPl > 60000UL) lastPl = 0; }
      // one fetch per loop pass, so the idle task (and its watchdog) gets CPU between TLS handshakes
      if (fwInstallReq) { fwInstallReq = false; fwInstall(); }
      else if (haToggleReq >= 0) { int i = haToggleReq; haToggleReq = -1; haToggle(i); uiRequestRedraw(); }
      else if (MOD_ON(M_BTC) && (!lastBtc || now - lastBtc > (uint32_t)cfg.btcEvery * 1000)) { fetchBtc(); lastBtc = millis(); uiRequestRedraw(); }
      else if (((MOD_ON(M_AGENDA) && !cfg.icalUrl.length()) || MOD_ON(M_NEWS) || MOD_ON(M_MAQ)) && cfg.feedUrl.length() && (!lastPl || now - lastPl > 5UL * 60 * 1000)) { fetchPlaca(); lastPl = millis(); uiRequestRedraw(); }
      else if (MOD_ON(M_AGENDA) && cfg.icalUrl.length() && timeIsSet() && (!lastIc || now - lastIc > 15UL * 60 * 1000)) { fetchIcal(); lastIc = millis(); uiRequestRedraw(); }
      else if (MOD_ON(M_CLIMA) && (!lastWx || now - lastWx > 30UL * 60 * 1000)) { fetchWeather(); lastWx = millis(); uiRequestRedraw(); }
      else if (MOD_ON(M_REDES) && (!lastSoc || now - lastSoc > 15UL * 60 * 1000)) { fetchSocial(); lastSoc = millis(); uiRequestRedraw(); }
      else if (MOD_ON(M_PIHOLE) && cfg.phUrl.length() && (!lastPh || now - lastPh > (curScreen == S_PIHOLE ? 15000UL : 120000UL))) { fetchPihole(); lastPh = millis(); uiRequestRedraw(); }
      else if (curScreen == S_CASA && (!lastHa || now - lastHa > 15000)) { fetchHa(); lastHa = millis(); uiRequestRedraw(); }
      else if (radioReq && (!lastRd || now - lastRd > 30UL * 60 * 1000)) { radioReq = false; fetchPropagation(); fetchIssPass(); lastRd = millis(); uiRequestRedraw(); }
      else if (radioReq) radioReq = false;
      else if (fwCheckReq || (now > 120000 && (!lastFw || now - lastFw > 12UL * 3600 * 1000))) { fwCheckReq = false; fwCheck(); lastFw = millis(); uiRequestRedraw(); }
      if (!hermesStarted && MOD_ON(M_AGENTE) && cfg.hermesUrl.length()) { hermesBegin(); hermesStarted = true; }
      if (hermesStarted) hermesLoop();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void netInit() {
  dataLock = xSemaphoreCreateMutex();
  WiFi.persistent(false);
  WiFi.setHostname(HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);
  if (anySsid()) joinBest();
  portalBegin();
  // TLS handshakes can keep core 0 busy for >5 s in one call, which trips the idle-task watchdog.
  // The UI task runs its own watchdog on this task instead (see netHeartbeat).
  disableCore0WDT();
  netHeartbeat = millis();
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 0);
}
