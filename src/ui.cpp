// UI: every screen is drawn in horizontal strips into one sprite and pushed to the
// panel, so updates never flicker. The same renderer feeds the serial screenshot.
#include "app.h"
#include "fonts.h"
#include "fonts_term.h"
#include <time.h>
#include <mbedtls/base64.h>
#include <WiFi.h>

volatile int curScreen = S_HOME;
static volatile bool redraw = true;
static LGFX_Sprite spr(&lcd);
static int stripH = 0;
static int yoff = 0;
static const uint8_t* curFont = nullptr;

// palette (RGB565)
#define RGB(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))
// Colors are roles; the theme fills them. C_WHITE is "text on an ink-filled block", C_PEACH "soft text on ink / soft fill".
static uint16_t C_BG, C_CARD, C_LINE, C_INK, C_MUTED, C_OR, C_ORT, C_GREEN, C_RED, C_BLUE, C_SOFT, C_PEACH, C_WARN, C_WHITE;
static bool term() { return cfg.theme != 0; }      // Terminal themes (window-titlebar look, monospace)
static bool anim() { return term() && cfg.anim; }
static void setPalette(uint32_t bg, uint32_t card, uint32_t line, uint32_t ink, uint32_t muted, uint32_t acc, uint32_t accText,
                       uint32_t green, uint32_t red, uint32_t blue, uint32_t soft, uint32_t peach, uint32_t warn, uint32_t onInk) {
  auto c = [](uint32_t v) { return RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF); };
  C_BG = c(bg); C_CARD = c(card); C_LINE = c(line); C_INK = c(ink); C_MUTED = c(muted); C_OR = c(acc); C_ORT = c(accText);
  C_GREEN = c(green); C_RED = c(red); C_BLUE = c(blue); C_SOFT = c(soft); C_PEACH = c(peach); C_WARN = c(warn); C_WHITE = c(onInk);
}
static void themePalette() {
  // Terminal palettes: green = action/ok, amber = alert, cyan = link/CTA.
  if (cfg.theme == 1)      setPalette(0x050706, 0x0D100E, 0x262C28, 0xE8ECE9, 0x7D8A82, 0x2FD675, 0x2FD675, 0x2FD675, 0xFFB020, 0x4FD8E0, 0x161A17, 0x167A41, 0xFFB020, 0x050706);
  else if (cfg.theme == 2) setPalette(0xF2F3EE, 0xFFFFFF, 0xDCDFD6, 0x14170F, 0x767C73, 0x167A41, 0x167A41, 0x167A41, 0xA15C06, 0x0B6E78, 0xECEEE8, 0x9FE3B8, 0xA15C06, 0xFFFFFF);
  else                     setPalette(0xF2F4F1, 0xFFFFFF, 0xDCE1DC, 0x16191D, 0x5B6570, 0xF7931A, 0xA9530A, 0x2B7A4B, 0xB23B2E, 0x2D5FA8, 0xECEFEB, 0xFCEBD7, 0x8A6A12, 0xFFFFFF);
}

// ---------- local toast ----------
static String toastMsg;
static uint32_t toastUntil = 0;
static void toast(const String& m, uint32_t ms = 2500) { toastMsg = m; toastUntil = millis() + ms; redraw = true; }

// ---------- notifications ----------
Notif notifs[NOTIF_MAX];
volatile int notifN = 0, notifUnread = 0;
void notifyAdd(uint8_t kind, const String& title, const String& text) {
  time_t now; time(&now);
  xSemaphoreTake(dataLock, portMAX_DELAY);
  for (int i = min((int)notifN, NOTIF_MAX - 1); i > 0; i--) notifs[i] = notifs[i - 1];
  notifs[0].kind = kind; notifs[0].title = title; notifs[0].text = text;
  notifs[0].at = now > 1700000000 ? (uint32_t)now : 0;
  if (notifN < NOTIF_MAX) notifN++;
  if (notifUnread < NOTIF_MAX) notifUnread++;
  xSemaphoreGive(dataLock);
  logf("[ui] aviso: %s · %s\n", title.c_str(), text.c_str());
  redraw = true;
}

// ---------- primitives (screen coords; strip offset applied here) ----------
static void rect(int x, int y, int w, int h, uint16_t c) { spr.fillRect(x, y - yoff, w, h, c); }
static void rrect(int x, int y, int w, int h, int r, uint16_t fill, uint16_t border) {
  spr.fillRoundRect(x, y - yoff, w, h, r, fill);
  if (border != fill) spr.drawRoundRect(x, y - yoff, w, h, r, border);
}
static void card(int x, int y, int w, int h) { rrect(x, y, w, h, term() ? 3 : 6, C_CARD, C_LINE); }
static void hline(int x, int y, int w, uint16_t c) { spr.drawFastHLine(x, y - yoff, w, c); }
static void circle(int x, int y, int r, uint16_t c) { spr.fillCircle(x, y - yoff, r, c); }
static void tri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t c) { spr.fillTriangle(x0, y0 - yoff, x1, y1 - yoff, x2, y2 - yoff, c); }

// Terminal themes swap every font for IBM Plex Mono at a size that keeps the same layout
static const uint8_t* tmap(const uint8_t* f) {
  if (!term()) return f;
  if (f == F_BM11) return T_BM11; if (f == F_BM13) return T_BM13; if (f == F_BS13) return T_BS13; if (f == F_BS16) return T_BS16;
  if (f == F_CS12) return T_CS12; if (f == F_CB18) return T_CB18; if (f == F_CB24) return T_CB24;
  if (f == F_CB34 || f == F_CB46) return T_CB34;
  return f;
}
static void font(const uint8_t* f) {
  f = tmap(f);
  if (curFont != f) { spr.unloadFont(); spr.loadFont(f); curFont = f; }
}
// lower case for titles, ASCII plus Latin-1 accents (UTF-8 C3 80..C3 9E)
static String lowerPt(const String& in) {
  String o = in;
  for (size_t i = 0; i < o.length(); i++) {
    uint8_t c = o[i];
    if (c >= 'A' && c <= 'Z') o.setCharAt(i, c + 32);
    else if (c == 0xC3 && i + 1 < o.length() && (uint8_t)o[i + 1] >= 0x80 && (uint8_t)o[i + 1] <= 0x9E && (uint8_t)o[i + 1] != 0x97) { o.setCharAt(i + 1, o[i + 1] + 0x20); i++; }
  }
  return o;
}
// ---------- terminal effects state (drawn straight on the panel between frames) ----------
static int curX = -1, curY = 0;            // blinking block cursor after the header title
static uint16_t curBg = 0;
static bool curOn = false;
static uint32_t lastBlink = 0, lastPulse = 0;
static int pulseN = 0, pulseX[3], pulseY[3];   // online machine dots on the home strip
static bool pulseOn = true;
static volatile bool screenChanged = true;
static void dots3(int x, int y) {          // window titlebar: three dots
  circle(x, y, 3, C_RED); circle(x + 10, y, 3, C_MUTED); circle(x + 20, y, 3, C_GREEN);
}
static int tw(const String& s, const uint8_t* f) { font(f); return spr.textWidth(s); }
// datum: 0 top-left, 1 top-center, 2 top-right
static void text(int x, int y, const String& s, const uint8_t* f, uint16_t fg, uint16_t bg, int datum = 0) {
  font(f);
  spr.setTextColor(fg, bg);
  spr.setTextDatum(datum == 1 ? textdatum_t::top_center : datum == 2 ? textdatum_t::top_right : textdatum_t::top_left);
  spr.drawString(s, x, y - yoff);
}
// pick the biggest font that fits
static const uint8_t* fit(const String& s, int maxW, const uint8_t* a, const uint8_t* b, const uint8_t* c = nullptr) {
  if (tw(s, a) <= maxW) return a;
  if (!c || tw(s, b) <= maxW) return b;
  return c;
}
// word wrap; returns number of lines drawn
static int wrap(int x, int y, int w, const String& s, const uint8_t* f, uint16_t fg, uint16_t bg, int lineH, int maxLines) {
  font(f);
  int lines = 0;
  String line, word;
  auto flush = [&](bool last) {
    if (lines < maxLines) {
      String out = line;
      if (lines == maxLines - 1 && !last) {
        while (out.length() && spr.textWidth(out + "…") > w) out.remove(out.length() - 1);
        out += "…";
      }
      text(x, y + lines * lineH, out, f, fg, bg);
    }
    lines++;
    line = "";
  };
  for (size_t i = 0; i <= s.length(); i++) {
    char c = i < s.length() ? s[i] : ' ';
    if (c == ' ' || c == '\n') {
      String cand = line.length() ? line + " " + word : word;
      if (spr.textWidth(cand) > w && line.length()) { flush(false); line = word; }
      else line = cand;
      word = "";
      if (c == '\n') flush(false);
    } else word += c;
  }
  if (line.length()) flush(true);
  return min(lines, maxLines);
}

// ---------- formatting ----------
static String fmtNum(double v, int dec) {
  char buf[32];
  bool neg = v < 0;
  if (neg) v = -v;
  long long ip = (long long)v;
  double frac = v - ip;
  char ib[24]; snprintf(ib, sizeof(ib), "%lld", ip);
  String s;
  int n = strlen(ib);
  for (int i = 0; i < n; i++) { s += ib[i]; if ((n - i - 1) % 3 == 0 && i < n - 1) s += '.'; }
  if (dec > 0) {
    snprintf(buf, sizeof(buf), "%.*f", dec, frac);
    String f = String(buf);
    if (f.startsWith("1")) { return fmtNum((neg ? -1 : 1) * (double)(ip + 1), dec); }
    s += ',';
    s += f.substring(2);
  }
  return neg ? "-" + s : s;
}
static String fmtBig(long v) {
  if (v < 0) return "—";
  if (v >= 1000000) return fmtNum(v / 1e6, v >= 10000000 ? 0 : 1) + " mi";
  if (v >= 10000) return fmtNum(v / 1e3, 0) + " mil";
  if (v >= 1000) return fmtNum(v / 1e3, 1) + " mil";
  return String(v);
}
static String fmtPct(float p) {
  String s = fmtNum(fabs(p), 2);
  return (p >= 0 ? "+" : "-") + s + "%";
}
static int savedCount();
static bool in(int x, int y, int rx, int ry, int rw, int rh);
static bool timeOk() { time_t t; time(&t); return t > 1700000000; }
static String hhmm(time_t t = 0) {
  if (!t) time(&t);
  struct tm tmv; localtime_r(&t, &tmv);
  char b[8]; snprintf(b, sizeof(b), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
  return b;
}
static const char* DIAS[7] = {"dom.", "seg.", "ter.", "qua.", "qui.", "sex.", "sáb."};
static const char* MESES[12] = {"jan.", "fev.", "mar.", "abr.", "mai.", "jun.", "jul.", "ago.", "set.", "out.", "nov.", "dez."};
static String dateStr() {
  time_t t; time(&t);
  struct tm tmv; localtime_r(&t, &tmv);
  return String(DIAS[tmv.tm_wday]) + ", " + tmv.tm_mday + " de " + MESES[tmv.tm_mon];
}
static String greeting() {
  if (!timeOk()) return "Olá";
  time_t t; time(&t);
  struct tm tmv; localtime_r(&t, &tmv);
  return tmv.tm_hour < 12 ? "Bom dia" : tmv.tm_hour < 18 ? "Boa tarde" : "Boa noite";
}
static String wxText(int c) {
  if (c == 0) return "Céu limpo";
  if (c <= 2) return "Parcialmente nublado";
  if (c == 3) return "Nublado";
  if (c == 45 || c == 48) return "Neblina";
  if (c >= 51 && c <= 57) return "Garoa";
  if (c >= 61 && c <= 67) return "Chuva";
  if (c >= 71 && c <= 77) return "Neve";
  if (c >= 80 && c <= 82) return "Pancadas de chuva";
  if (c >= 95) return "Tempestade";
  return "—";
}
static String wxShort(int c) {
  if (c == 0) return "Sol";
  if (c <= 2) return "Sol e nuvens";
  if (c == 3) return "Nublado";
  if (c == 45 || c == 48) return "Neblina";
  if ((c >= 51 && c <= 67) || (c >= 80 && c <= 82)) return "Chuva";
  if (c >= 95) return "Tempestade";
  return "—";
}

// ---------- icons ----------
static void iconWeather(int x, int y, int s, int code) {  // s = size
  bool sun = code <= 2, cloud = code >= 1, rain = (code >= 51 && code <= 67) || (code >= 80 && code <= 82) || code >= 95;
  if (sun) {
    int cx = x + s * 2 / 5, cy = y + s * 2 / 5, r = s / 5;
    for (int a = 0; a < 8; a++) {
      float ang = a * PI / 4;
      int x1 = cx + cos(ang) * (r + 3), y1 = cy + sin(ang) * (r + 3), x2 = cx + cos(ang) * (r + s / 8 + 3), y2 = cy + sin(ang) * (r + s / 8 + 3);
      spr.drawLine(x1, y1 - yoff, x2, y2 - yoff, C_ORT);
    }
    circle(cx, cy, r, C_OR);
  }
  if (cloud) {
    int cy = y + s * 3 / 5;
    uint16_t c = rain ? C_MUTED : RGB(0xB8, 0xC0, 0xC6);
    circle(x + s * 2 / 5, cy, s / 5, c);
    circle(x + s * 3 / 5, cy - s / 10, s / 4, c);
    circle(x + s * 4 / 5, cy + 2, s / 6, c);
    rect(x + s * 2 / 5, cy, s * 2 / 5, s / 6, c);
  }
  if (rain) for (int i = 0; i < 3; i++) spr.drawLine(x + s * 2 / 5 + i * s / 6, y + s * 4 / 5 + 4 - yoff, x + s * 2 / 5 + i * s / 6 - 3, y + s - yoff, C_BLUE);
}

// ---------- header ----------
static void header(const char* title, bool back = true, const char* backTo = "Início") {
  if (term()) {
    rect(0, 0, 320, 23, C_CARD);
    hline(0, 23, 320, C_LINE);
    dots3(10, 11);
    int x = 38;
    if (back) { String b = String("‹ ") + lowerPt(backTo); text(x, 5, b, F_BS13, C_BLUE, C_CARD); x += tw(b, F_BS13) + 8; }
    String t = "~/" + lowerPt(title);
    text(x, 5, t, F_BS13, C_INK, C_CARD);
    curX = x + tw(t, F_BS13) + 2; curY = 5; curBg = C_CARD;
    String right = timeOk() ? hhmm() : (wifiUp ? "sync" : (apMode ? "config" : "offline"));
    text(312, 5, right, F_BS13, wifiUp ? C_MUTED : C_RED, C_CARD, 2);
    return;
  }
  rect(0, 0, 320, 22, C_CARD);
  rect(0, 22, 320, 2, C_OR);
  int x = 8;
  if (back) {
    String b = String("‹ ") + backTo;
    text(x, 4, b, F_BS13, C_BLUE, C_CARD);
    x += tw(b, F_BS13) + 10;
  }
  text(x, 5, title, F_CS12, C_INK, C_CARD);
  String right = timeOk() ? hhmm() : (wifiUp ? "sincronizando" : (apMode ? "modo config" : "sem Wi-Fi"));
  text(312, 5, right, F_CS12, wifiUp ? C_INK : C_ORT, C_CARD, 2);
}
static bool hitBack(int x, int y) { return y < 26 && x < 80; }

// ---------- agenda time labels ----------
static String whenLabel(uint32_t q, bool diaTodo) {
  if (!q) return "—";
  time_t t = q, now; time(&now);
  struct tm a, b; localtime_r(&t, &a); localtime_r(&now, &b);
  char hm[8]; snprintf(hm, sizeof(hm), "%02d:%02d", a.tm_hour, a.tm_min);
  long dd = (long)((q / 86400) - ((uint32_t)now / 86400));
  // compare local calendar days
  int dayA = a.tm_year * 400 + a.tm_yday, dayB = b.tm_year * 400 + b.tm_yday;
  dd = dayA - dayB;
  String hora = diaTodo ? String("") : String(" ") + hm;
  if (dd == 0) return "hoje" + hora;
  if (dd == 1) return "amanhã" + hora;
  char dm[12]; snprintf(dm, sizeof(dm), "%02d/%02d", a.tm_mday, a.tm_mon + 1);
  return String(DIAS[a.tm_wday]) + " " + dm + hora;
}
static String countdown(uint32_t q) {
  if (!q) return "";
  time_t now; time(&now);
  long s = (long)q - (long)now;
  if (s < -3600) return "passou";
  if (s < 0) return "agora";
  if (s < 3600) return "em " + String(s / 60) + " min";
  if (s < 86400) return "em " + String(s / 3600) + " h";
  return "em " + String((s + 43200) / 86400) + " dias";
}
static uint16_t saudeColor() {
  if (live.saude == "ok") return C_GREEN;
  if (live.saude == "alerta") return C_WARN;
  if (live.saude == "critico") return C_RED;
  return C_LINE;
}

// ---------- home layout: up to 4 slots chosen in Config › Início ----------
enum { HI_NONE = 0, HI_CLOCK, HI_BTC, HI_CLIMA, HI_REDES, HI_PIHOLE, HI_MAQ, HI_AGENDA, HI_IA, HI_COUNT };
static const char* const HI_NAME[HI_COUNT] = {"", "Relógio", "Bitcoin", "Clima", "Seguidores", "Pi-hole", "Máquinas", "Agenda", "Uso de IA"};
static int hiCost(int it) { return it == HI_MAQ ? 2 : 1; }          // machines take a full row
static bool hiAvailable(int it) {
  switch (it) {
    case HI_BTC: return MOD_ON(M_BTC); case HI_CLIMA: return MOD_ON(M_CLIMA); case HI_REDES: return MOD_ON(M_REDES);
    case HI_PIHOLE: return MOD_ON(M_PIHOLE); case HI_MAQ: case HI_IA: return MOD_ON(M_MAQ); case HI_AGENDA: return MOD_ON(M_AGENDA);
    default: return it == HI_CLOCK;
  }
}
static int hiScreen(int it) {
  switch (it) {
    case HI_CLOCK: case HI_AGENDA: return S_AGENDA; case HI_BTC: return S_BTC; case HI_CLIMA: return S_CLIMA;
    case HI_REDES: return S_REDES; case HI_PIHOLE: return S_PIHOLE; case HI_MAQ: case HI_IA: return S_MAQ;
  }
  return -1;
}
static int homePick(int i) { return (cfg.homeItems >> (i * 4)) & 0xF; }
static int homePickN() { int n = 0; while (n < 4 && homePick(n)) n++; return n; }
static int homeSlot[4];                 // item in each slot (A top-left, B top-right, C, D); -1 = covered by a wide item
static const int SLOT_X[4] = {6, 163, 6, 163}, SLOT_Y[4] = {30, 30, 112, 112};
static int homeDock[6]; static int homeDockN = 0;
static bool homeHas(int it) { for (int k = 0; k < 4; k++) if (homeSlot[k] == it) return true; return false; }
static void homeLayout() {
  for (int k = 0; k < 4; k++) homeSlot[k] = HI_NONE;
  int n = homePickN();
  // machines first: a whole row (the second one), the rest fill the free slots in the chosen order
  for (int i = 0; i < n; i++) if (homePick(i) == HI_MAQ && hiAvailable(HI_MAQ)) { homeSlot[2] = HI_MAQ; homeSlot[3] = -1; }
  for (int i = 0; i < n; i++) {
    int it = homePick(i);
    if (it == HI_MAQ || !hiAvailable(it)) continue;
    for (int k = 0; k < 4; k++) if (homeSlot[k] == HI_NONE) { homeSlot[k] = it; break; }
  }
  // dock: by priority, the agent always last; more than fits -> "Mais" (all apps)
  const int pri[10] = {S_REDES, S_CLIMA, S_PIHOLE, S_CASA, S_JOGOS, S_AGENDA, S_NEWS, S_MAQ, S_IR, S_MESH};
  const int priMod[10] = {M_REDES, M_CLIMA, M_PIHOLE, M_CASA, M_JOGOS, M_AGENDA, M_NEWS, M_MAQ, M_IR, M_MESH};
  int items[12], n2 = 0;
  if (deckFresh()) items[n2++] = S_DECK;   // cyberdeck host connected over USB
  for (int i = 0; i < 10; i++) {
    if (!MOD_ON(priMod[i])) continue;
    bool onHome = false;
    for (int k = 0; k < 4; k++) if (homeSlot[k] > 0 && hiScreen(homeSlot[k]) == pri[i]) onHome = true;
    if (onHome) continue;
    items[n2++] = pri[i];
  }
  bool jar = MOD_ON(M_AGENTE);
  int cap = 5 - (jar ? 1 : 0);
  homeDockN = 0;
  if (n2 > cap) { for (int i = 0; i < cap - 1; i++) homeDock[homeDockN++] = items[i]; homeDock[homeDockN++] = S_APPS; }
  else for (int i = 0; i < n2; i++) homeDock[homeDockN++] = items[i];
  if (jar) homeDock[homeDockN++] = S_AGENTE;
}
static String dockLabel(int s) {
  switch (s) {
    case S_CASA: return "Casa"; case S_JOGOS: return "Jogos"; case S_IR: return "Controle";
    case S_MESH: return "Mesh"; case S_PIHOLE: return "Pi-hole"; case S_AGENTE: return "Agente";
    case S_DECK: return "Deck"; case S_NEWS: return "Notícias"; case S_MAQ: return "Máquinas"; case S_APPS: return "Mais";
    case S_AGENDA: return "Agenda"; case S_BTC: return "Bitcoin"; case S_CLIMA: return "Clima"; case S_REDES: return "Seguidores";
    case S_RADIO: return "Rádio"; case S_AVISOS: return "Avisos"; case S_APAGAO: return "Apagão"; case S_SSH: return "Terminal";
  }
  return "?";
}
static void homeCard(int which, int x, int y) {
  card(x, y, 151, 76);
  int tx = x + 8;
  switch (which) {
    case S_BTC:
      circle(tx + 2, y + 11, 3, C_OR);
      text(tx + 9, y + 5, "BITCOIN", F_CS12, C_ORT, C_CARD);
      if (live.btcOk) {
        String p = "US$ " + fmtNum(live.btc, 0);
        text(tx, y + 19, p, fit(p, 137, F_CB24, F_CB18), C_INK, C_CARD);
        bool up = live.btcChg >= 0;
        uint16_t cc = up ? C_GREEN : C_RED;
        if (up) tri(tx, y + 54, tx + 8, y + 54, tx + 4, y + 47, cc); else tri(tx, y + 47, tx + 8, y + 47, tx + 4, y + 54, cc);
        text(tx + 12, y + 44, fmtPct(live.btcChg) + " 24h", F_BS13, cc, C_CARD);
        text(tx, y + 59, "US$ 1 = " + fmtNum(1e8 / live.btc, 0) + " sats", F_BM11, C_MUTED, C_CARD);
      } else text(tx, y + 22, wifiUp ? "carregando…" : "sem Wi-Fi", F_BS13, C_MUTED, C_CARD);
      break;
    case S_CLIMA:
      text(tx, y + 5, "CLIMA", F_CS12, C_MUTED, C_CARD);
      if (live.wxOk) {
        iconWeather(x + 110, y + 6, 34, live.wxCode);
        text(tx, y + 19, fmtNum(live.wxTemp, 0) + "°", F_CB34, C_INK, C_CARD);
        text(tx, y + 57, wxText(live.wxCode), F_BM11, C_MUTED, C_CARD);
      } else text(tx, y + 28, wifiUp ? "carregando…" : "sem Wi-Fi", F_BS13, C_MUTED, C_CARD);
      break;
    case S_REDES:
      text(tx, y + 5, "SEGUIDORES", F_CS12, C_MUTED, C_CARD);
      if (live.socOk) {
        text(tx, y + 19, fmtBig(live.socTotal), F_CB34, C_INK, C_CARD);
        text(tx, y + 57, "somando todas as redes", F_BM11, C_MUTED, C_CARD);
      } else {
        text(tx, y + 24, wifiUp ? "aguardando n8n" : "sem Wi-Fi", F_BS13, C_MUTED, C_CARD);
        text(tx, y + 44, "dados vêm do seu feed", F_BM11, C_MUTED, C_CARD);
      }
      break;
    case S_PIHOLE:
      text(tx, y + 5, "PI-HOLE", F_CS12, C_MUTED, C_CARD);
      if (live.phOk) {
        text(tx, y + 19, fmtNum(live.phPct, 1) + "%", F_CB34, C_INK, C_CARD);
        text(tx, y + 57, fmtBig(live.phBlocked) + " bloqueadas hoje", F_BM11, C_MUTED, C_CARD);
      } else {
        String m = cfg.phUrl.length() ? (live.phErr.length() ? live.phErr : String("carregando…")) : String("não configurado");
        text(tx, y + 24, m, fit(m, 135, F_BS13, F_BM11), C_MUTED, C_CARD);
        if (!cfg.phUrl.length()) text(tx, y + 44, "Config › Pelo celular", F_BM11, C_MUTED, C_CARD);
      }
      break;
    default:
      text(x + 75, y + 30, "—", F_BS16, C_LINE, C_CARD, 1);
      break;
  }
}

// ---------- machines (from the feed) ----------
static uint16_t levelColor(int v, int warn, int bad) { return v >= bad ? C_RED : v >= warn ? C_OR : C_GREEN; }
static String agoShort(uint32_t s) {
  if (s < 120) return String(s) + " s";
  if (s < 7200) return String(s / 60) + " min";
  if (s < 172800) return String(s / 3600) + " h";
  return String(s / 86400) + " d";
}
static void meter(int x, int y, int w, const char* lab, int v, int warn, int bad) {
  text(x, y, lab, F_BM11, C_MUTED, C_CARD);
  int bx = x + 30, bw = w - 30 - 30;
  rrect(bx, y + 5, bw, 5, 2, C_SOFT, C_SOFT);
  if (v >= 0) rrect(bx, y + 5, max(4, bw * min(v, 100) / 100), 5, 2, levelColor(v, warn, bad), levelColor(v, warn, bad));
  text(x + w, y, v >= 0 ? String(v) + "%" : String("—"), F_BM11, v >= bad ? C_RED : C_INK, C_CARD, 2);
}
static void drawMachinesStrip(int x, int y, int w, int h) {
  card(x, y, w, h);
  int n = min(live.mqN, 3);
  if (!n) {
    text(x + 10, y + 7, "MÁQUINAS", F_CS12, C_MUTED, C_CARD);
    String m = !cfg.feedUrl.length() ? String("Configure a URL do feed em Config › Pelo celular") : live.plErr.length() ? live.plErr : String("aguardando o feed…");
    wrap(x + 10, y + 26, w - 20, m, F_BM13, C_MUTED, C_CARD, 16, 2);
    return;
  }
  int cw = (w - 8) / n;
  for (int i = 0; i < n; i++) {
    int cx = x + 6 + i * cw, inner = cw - 10;
    if (i) spr.drawFastVLine(cx - 3, y + 8 - yoff, h - 16, C_SOFT);
    bool on = live.mqOn[i];
    circle(cx + 5, y + 13, 4, on ? C_GREEN : C_RED);
    if (on && pulseN < 3) { pulseX[pulseN] = cx + 5; pulseY[pulseN] = y + 13; pulseN++; }
    String nm = live.mqNome[i];
    text(cx + 13, y + 6, nm, fit(nm, inner - 44, F_BS13, F_BM11), C_INK, C_CARD);
    if (on && live.mqTemp[i] >= 0) text(cx + inner + 2, y + 7, String(live.mqTemp[i]) + "°", F_BM11, live.mqTemp[i] >= 80 ? C_RED : C_MUTED, C_CARD, 2);
    if (!on) {
      text(cx + 2, y + 28, "sem sinal", F_BS13, C_RED, C_CARD);
      text(cx + 2, y + 46, "há " + agoShort(live.mqAge[i]), F_BM11, C_MUTED, C_CARD);
      continue;
    }
    meter(cx + 2, y + 26, inner, "CPU", live.mqCpu[i], 75, 90);
    meter(cx + 2, y + 41, inner, "RAM", live.mqRam[i], 80, 92);
    meter(cx + 2, y + 56, inner, "disco", live.mqDisk[i], 85, 95);
  }
}

static void drawClockCard(int x, int y) {
  card(x, y, 151, 76);
  int tx = x + 8;
  text(tx, y + 6, timeOk() ? hhmm() : "--:--", F_CB34, C_INK, C_CARD);
  text(tx, y + 44, timeOk() ? dateStr() : "aguardando hora", F_BM11, C_MUTED, C_CARD);
  if (MOD_ON(M_CLIMA) && !homeHas(HI_CLIMA) && live.wxOk) {   // small weather; tap it to open Clima
    iconWeather(x + 116, y + 4, 28, live.wxCode);
    text(x + 143, y + 34, fmtNum(live.wxTemp, 0) + "°", F_BS13, C_INK, C_CARD, 2);
  }
  if (MOD_ON(M_AGENDA) && live.agN > 0 && !homeHas(HI_AGENDA)) {
    String a = "› " + live.agT[0];
    String c = countdown(live.agQ[0]);
    int maxw = 136 - tw(" · " + c, F_BM11);
    while (a.length() > 3 && tw(a, F_BS13) > maxw) a.remove(a.length() - 1);
    text(tx, y + 58, a, F_BS13, C_ORT, C_CARD);
    text(tx + tw(a, F_BS13), y + 60, " · " + c, F_BM11, C_MUTED, C_CARD);
  } else {
    String c = live.wxPlace.length() ? live.wxPlace : (cfg.city.length() ? cfg.city : String(PROJECT_NAME));
    c.toUpperCase();
    text(tx, y + 59, c, fit(c, 136, F_CS12, F_BM11), C_ORT, C_CARD);
  }
}
static void drawAgendaCard(int x, int y) {
  card(x, y, 151, 76);
  int tx = x + 8;
  text(tx, y + 5, "AGENDA", F_CS12, C_ORT, C_CARD);
  if (!live.agN) { String e = cfg.icalUrl.length() ? live.icErr : live.plErr; wrap(tx, y + 24, 135, e.length() ? e : String("Nada marcado"), F_BM13, C_MUTED, C_CARD, 16, 2); return; }
  wrap(tx, y + 21, 135, live.agT[0], F_BS13, C_INK, C_CARD, 16, 2);
  text(tx, y + 58, countdown(live.agQ[0]), F_BM11, C_MUTED, C_CARD);
  if (live.agN > 1) text(x + 143, y + 58, "+" + String(live.agN - 1), F_BM11, C_MUTED, C_CARD, 2);
}
static void drawIaCard(int x, int y) {
  card(x, y, 151, 76);
  int tx = x + 8;
  text(tx, y + 5, "USO DE IA · 30 DIAS", F_CS12, C_ORT, C_CARD);
  if (live.iaCusto < 0) { text(tx, y + 28, live.plErr.length() ? live.plErr : String("aguardando o feed"), fit("aguardando o feed", 135, F_BM13, F_BM11), C_MUTED, C_CARD); return; }
  String v = "US$ " + fmtNum(live.iaCusto, 0);
  text(tx, y + 20, v, fit(v, 137, F_CB24, F_CB18), C_INK, C_CARD);
  if (live.iaTokens >= 0) {
    String t = live.iaTokens >= 1e9 ? fmtNum(live.iaTokens / 1e9, 1) + " bi tokens" : fmtNum(live.iaTokens / 1e6, 0) + " mi tokens";
    text(tx, y + 56, t, F_BM11, C_MUTED, C_CARD);
  }
}

// ---------- screens ----------
static String homeAlert() {         // what the top bar shows instead of the greeting, "" = greeting
  if (hermes.promptId.length()) return "aprovação pendente";
  if (!netOnline && offlineSince) return "sem internet";
  if (notifUnread) return String(notifUnread) + (notifUnread == 1 ? " aviso novo" : " avisos novos");
  return "";
}
static void drawHome() {
  String al = homeAlert();
  if (term()) {
    rect(0, 0, 320, 23, C_CARD);
    hline(0, 23, 320, C_LINE);
    dots3(10, 11);
    String g = al.length() ? "! " + al : lowerPt(greeting() + (cfg.nome.length() ? ", " + cfg.nome : String("")));
    text(38, 5, g, F_BS13, al.length() ? C_WARN : C_INK, C_CARD);
    curX = 38 + tw(g, F_BS13) + 2; curY = 5; curBg = C_CARD;
  } else {
    rect(0, 0, 320, 22, C_CARD);
    rect(0, 22, 320, 2, C_OR);
    rrect(8, 4, 30, 15, 3, C_OR, C_OR);
    text(23, 5, "CD", F_CS12, C_INK, C_OR, 1);
    if (al.length()) {
      String t = al; t.setCharAt(0, toupper(t[0]));
      int w = tw(t, F_BS13) + 18;
      rrect(44, 3, w, 17, 4, C_WARN, C_WARN);
      text(44 + w / 2, 4, t, F_BS13, C_WHITE, C_WARN, 1);
    } else text(44, 4, greeting() + (cfg.nome.length() ? ", " + cfg.nome : String("")), F_BS13, C_INK, C_CARD);
  }
  String cfgLab = term() ? "config" : "Config";
  text(312, 4, cfgLab, F_BS13, C_BLUE, C_CARD, 2);
  int cw = tw(cfgLab, F_BS13);
  circle(312 - cw - 10, 12, 3, wifiUp ? C_GREEN : (apMode ? C_BLUE : C_RED));
  if (MOD_ON(M_AGENTE) && live.saude.length()) {   // server health (from the feed)
    text(312 - cw - 30, 5, "S", F_CS12, saudeColor(), C_CARD, 1);
  }

  homeLayout();
  if (!homeHas(HI_CLOCK) && timeOk()) text(312 - cw - 42, 5, hhmm(), F_BS13, C_INK, C_CARD, 2);   // clock moves to the top bar
  for (int k = 0; k < 4; k++) {
    int it = homeSlot[k], x = SLOT_X[k], y = SLOT_Y[k];
    if (it <= 0) continue;
    if (it == HI_MAQ) drawMachinesStrip(6, y, 308, 76);
    else if (it == HI_CLOCK) drawClockCard(x, y);
    else if (it == HI_AGENDA) drawAgendaCard(x, y);
    else if (it == HI_IA) drawIaCard(x, y);
    else homeCard(hiScreen(it), x, y);
  }
  if (!homePickN()) text(160, 100, "Escolha os itens em Config › Início", F_BS13, C_MUTED, C_BG, 1);

  // dock: enabled shortcuts (pi-hole too when it didn't get a card)
  int n = homeDockN;
  if (!n) { text(160, 206, "Ative módulos em Config › Módulos", F_BM11, C_MUTED, C_BG, 1); return; }
  int gap = 5, w = (306 - (n - 1) * gap) / n;
  for (int i = 0; i < n; i++) {
    int x = 7 + i * (w + gap);
    bool dark = homeDock[i] == S_AGENTE;
    rrect(x, 194, w, 40, 6, dark ? C_INK : C_CARD, dark ? C_INK : C_LINE);
    String lab = dockLabel(homeDock[i]);
    text(x + w / 2, 206, lab, fit(lab, w - 6, F_BS13, F_BM11), dark ? C_WHITE : C_INK, dark ? C_INK : C_CARD, 1);
  }
}

static void drawBtc() {
  header("BITCOIN");
  card(8, 32, 210, 84);
  text(16, 38, "BTC / USD", F_CS12, C_ORT, C_CARD);
  if (live.btcOk) {
    text(16, 52, fmtNum(live.btc, 0), F_CB46, C_INK, C_CARD);
    bool up = live.btcChg >= 0;
    uint16_t cc = up ? C_GREEN : C_RED;
    if (up) tri(16, 104, 25, 104, 20.5, 96, cc); else tri(16, 96, 25, 96, 20.5, 104, cc);
    text(30, 94, fmtPct(live.btcChg) + " em 24h", F_BS13, cc, C_CARD);
  } else text(16, 64, wifiUp ? "carregando…" : "sem Wi-Fi", F_BS16, C_MUTED, C_CARD);

  card(224, 32, 88, 84);
  text(268, 38, "Medo & Ganância", F_BM11, C_MUTED, C_CARD, 1);
  if (live.fng >= 0) {
    // gauge bar
    rrect(234, 82, 68, 6, 3, C_SOFT, C_SOFT);
    rrect(234, 82, max(6, 68 * live.fng / 100), 6, 3, C_OR, C_OR);
    text(268, 52, String(live.fng), F_CB24, C_INK, C_CARD, 1);
    text(268, 94, live.fngLabel, F_BS13, C_ORT, C_CARD, 1);
  } else text(268, 64, "—", F_CB24, C_MUTED, C_CARD, 1);

  card(8, 122, 304, 50);
  text(16, 127, "TAXAS ON-CHAIN · sat/vB", F_CS12, C_MUTED, C_CARD);
  const char* lab[3] = {"Lenta", "Normal", "Rápida"};
  int vals[3] = {live.feeSlow, live.feeStd, live.feeFast};
  for (int i = 0; i < 3; i++) {
    int x = 16 + i * 98;
    rrect(x, 145, 90, 20, 4, i == 2 ? C_PEACH : C_BG, i == 2 ? C_PEACH : C_BG);
    text(x + 6, 148, lab[i], F_BM11, C_INK, i == 2 ? C_PEACH : C_BG);
    text(x + 84, 147, live.btcOk ? String(vals[i]) : "—", F_BS13, C_INK, i == 2 ? C_PEACH : C_BG, 2);
  }

  auto mini = [&](int x, const char* sym, float p, float c) {
    card(x, 178, 149, 30);
    text(x + 8, 185, sym, F_BS13, C_INK, C_CARD);
    text(x + 40, 185, p > 0 ? fmtNum(p, p < 1000 ? 2 : 0) : "—", F_BS13, C_INK, C_CARD);
    if (p > 0) text(x + 141, 185, fmtPct(c), F_BS13, c >= 0 ? C_GREEN : C_RED, C_CARD, 2);
  };
  mini(8, "ETH", live.eth, live.ethChg);
  mini(163, "SOL", live.sol, live.solChg);
  String foot = live.btcOk ? ("US$ 1 = " + fmtNum(1e8 / live.btc, 0) + " sats · CoinGecko · " + hhmm(live.btcAt)) : "Fontes: CoinGecko, mempool.space, alternative.me";
  text(8, 220, foot, F_BM11, C_MUTED, C_BG);
}

static void drawClima() {
  header("CLIMA");
  card(8, 32, 304, 90);
  if (live.wxOk) {
    iconWeather(16, 44, 56, live.wxCode);
    text(84, 40, fmtNum(live.wxTemp, 0) + "°", F_CB46, C_INK, C_CARD);
    int x = 84 + tw(fmtNum(live.wxTemp, 0) + "°", F_CB46) + 14;
    text(x, 46, wxText(live.wxCode), F_BS13, C_INK, C_CARD);
    text(x, 66, "Umidade " + String(live.wxHum) + "%", F_BM11, C_MUTED, C_CARD);
    text(x, 82, "Vento " + fmtNum(live.wxWind, 0) + " km/h", F_BM11, C_MUTED, C_CARD);
    for (int i = 1; i <= 3; i++) {
      int x0 = 8 + (i - 1) * 104;
      card(x0, 128, 96, 82);
      text(x0 + 48, 133, live.dDay[i], F_BS13, C_INK, C_CARD, 1);
      iconWeather(x0 + 33, 150, 30, live.dCode[i]);
      text(x0 + 48, 186, fmtNum(live.dMax[i], 0) + "° / " + fmtNum(live.dMin[i], 0) + "°", F_BS13, C_INK, C_CARD, 1);
    }
  } else {
    text(160, 66, wifiUp ? (cfg.city.length() ? "carregando…" : "localizando pelo IP…") : "sem Wi-Fi", F_BS16, C_MUTED, C_CARD, 1);
  }
  text(8, 220, "Open-Meteo · " + (live.wxPlace.length() ? live.wxPlace : cfg.city) + (live.wxByIp ? " (pelo IP)" : "") + (live.wxOk ? " · " + hhmm(live.wxAt) : ""), F_BM11, C_MUTED, C_BG);
}

static void drawRedes() {
  header("REDES SOCIAIS");
  rrect(8, 32, 304, 46, 6, C_INK, C_INK);
  text(18, 38, live.socOk ? fmtBig(live.socTotal) : "—", F_CB34, C_WHITE, C_INK);
  text(150, 44, "seguidores somados", F_BM11, C_WHITE, C_INK);
  text(150, 58, "em todas as redes", F_BM11, C_WHITE, C_INK);
  card(8, 84, 304, 128);
  const char* nets[6] = {"Instagram", "TikTok", "Facebook", "Threads", "YouTube", "X"};
  long vals[6] = {live.socIg, live.socTt, live.socFb, live.socTh, live.socYt, live.socX};
  for (int i = 0; i < 6; i++) {
    int y = 84 + i * 21;
    if (i) hline(9, y, 302, C_SOFT);
    text(18, y + 4, nets[i], F_BS13, C_INK, C_CARD);
    bool has = live.socOk && vals[i] >= 0;
    text(302, y + 4, has ? fmtNum(vals[i], 0) : "—", F_BS13, has ? C_INK : C_MUTED, C_CARD, 2);
  }
  String foot = live.socOk ? "feed · atualizado " + hhmm(live.socAt) : (cfg.n8nUrl.length() ? "Aguardando dados" : "Configure a URL dos seguidores em Config › Pelo celular");
  text(8, 222, foot, F_BM11, C_MUTED, C_BG);
}

static void drawCasa() {
  header("CASA · HOME ASSISTANT");
  bool conf = cfg.haUrl.length() && cfg.haToken.length();
  for (int i = 0; i < 4; i++) {
    int x = 8 + (i % 2) * 156, y = 32 + (i / 2) * 70;
    int st = live.haState[i];
    bool on = st == 1;
    rrect(x, y, 148, 64, 6, on ? C_INK : C_CARD, on ? C_INK : C_LINE);
    text(x + 10, y + 10, cfg.haName[i], F_BS13, on ? C_WHITE : C_INK, on ? C_INK : C_CARD);
    String s = !conf || !cfg.haEnt[i].length() ? "não configurado" : st < 0 ? "—" : on ? "Ligado" : "Desligado";
    text(x + 10, y + 40, s, F_BM11, on ? C_WHITE : C_MUTED, on ? C_INK : C_CARD);
  }
  String foot = !conf ? "Configure o Home Assistant em Config › Pelo celular" : (live.haErr.length() ? live.haErr : "Toque num botão pra ligar ou desligar");
  text(8, 182, foot, F_BM11, C_MUTED, C_BG);
}

static uint32_t aprovArmUntil = 0;   // first tap on "Aprovar" arms it, a second one within 4 s answers
static void drawAgente() {
  header("AGENTE · HERMES");
  if (hermes.promptId.length()) {            // a question waiting for you comes before anything else
    card(8, 30, 304, 160);
    text(18, 36, "APROVAÇÃO PEDIDA PELO AGENTE", F_CS12, C_WARN, C_CARD);
    if (hermes.promptUntil) {
      long left = max(0L, (long)((int32_t)(hermes.promptUntil - millis())) / 1000);
      char b[16]; snprintf(b, sizeof(b), "%ld:%02ld", left / 60, left % 60);
      text(302, 36, String("expira em ") + b, F_BM11, C_MUTED, C_CARD, 2);
    }
    text(18, 54, hermes.promptTitle, fit(hermes.promptTitle, 284, F_BS16, F_BS13), C_INK, C_CARD);
    wrap(18, 78, 284, hermes.promptText, F_BM13, C_INK, C_CARD, 17, 6);
    bool armed = millis() < aprovArmUntil;
    rrect(8, 196, 148, 38, 6, armed ? C_WARN : C_INK, armed ? C_WARN : C_INK);
    text(82, 206, armed ? "Toque de novo" : "Aprovar", F_BS16, C_WHITE, armed ? C_WARN : C_INK, 1);
    rrect(164, 196, 148, 38, 6, C_CARD, C_LINE);
    text(238, 206, "Recusar", F_BS16, C_INK, C_CARD, 1);
    return;
  }
  if (!cfg.hermesUrl.length()) {
    card(8, 32, 304, 120);
    text(18, 42, "Agente ainda não configurado", F_BS16, C_INK, C_CARD);
    wrap(18, 66, 284, "No host do Hermes, rode: hermes gadget info. Copie o endereço ws:// e cole em Config › Pelo celular.", F_BM13, C_MUTED, C_CARD, 17, 4);
    return;
  }
  if (hermes.status <= 1 || hermes.status == 4) {
    card(8, 32, 304, 90);
    text(18, 42, hermes.status == 4 ? "Erro na conexão" : (wifiUp ? "Conectando ao Hermes…" : "Sem Wi-Fi"), F_BS16, C_INK, C_CARD);
    wrap(18, 66, 284, hermes.status == 4 ? hermes.err : cfg.hermesUrl, F_BM11, C_MUTED, C_CARD, 15, 3);
    return;
  }
  if (hermes.status == 2) {
    card(8, 32, 304, 150);
    text(18, 40, "Aprove este aparelho no Hermes", F_BS13, C_INK, C_CARD);
    text(160, 64, hermes.pairCode.length() ? hermes.pairCode : "…", F_CB34, C_ORT, C_CARD, 1);
    wrap(18, 112, 284, "No host do Hermes: hermes pairing approve gadget " + hermes.pairCode, F_BM13, C_MUTED, C_CARD, 17, 3);
    text(8, 220, "Aparelho " + hermes.deviceId, F_BM11, C_MUTED, C_BG);
    return;
  }
  // paired
  card(8, 32, 304, 178);
  text(18, 40, hermes.statusLine.length() ? hermes.statusLine : "ÚLTIMA MENSAGEM", F_CS12, C_MUTED, C_CARD);
  wrap(18, 60, 284, hermes.lastReply.length() ? hermes.lastReply : "Nenhuma mensagem ainda. Peça algo ao seu agente e mande pra tela, ou espere os avisos dele.", F_BM13, C_INK, C_CARD, 18, 8);
  text(8, 220, "Pareado · modo texto (sem microfone)", F_BM11, C_GREEN, C_BG);
}

// ---------- games ----------
static const int SN_COLS = 20, SN_ROWS = 13, SN_CELL = 16, SN_Y = 26;
static int8_t snX[260], snY[260];
static int snLen = 0, snDir = 0, snNext = 0, foodX = 0, foodY = 0, snScore = 0;
static bool snDead = false;
static uint32_t snTick = 0;
static void snakeReset() {
  snLen = 4; snDir = snNext = 0; snScore = 0; snDead = false;
  for (int i = 0; i < snLen; i++) { snX[i] = 8 - i; snY[i] = 6; }
  foodX = 14; foodY = 6;
}
static void snakeStep() {
  if (snDead) return;
  snDir = snNext;
  int dx[4] = {1, 0, -1, 0}, dy[4] = {0, 1, 0, -1};
  int nx = snX[0] + dx[snDir], ny = snY[0] + dy[snDir];
  if (nx < 0 || ny < 0 || nx >= SN_COLS || ny >= SN_ROWS) { snDead = true; }
  for (int i = 0; i < snLen && !snDead; i++) if (snX[i] == nx && snY[i] == ny) snDead = true;
  if (snDead) {
    if (snScore > cfg.snakeBest) { cfg.snakeBest = snScore; configSave(); }
    beep(600, 200);
    return;
  }
  bool eat = nx == foodX && ny == foodY;
  if (eat && snLen < 259) snLen++;
  for (int i = snLen - 1; i > 0; i--) { snX[i] = snX[i - 1]; snY[i] = snY[i - 1]; }
  snX[0] = nx; snY[0] = ny;
  if (eat) {
    snScore++;
    beep(2800, 25);
    for (int t = 0; t < 100; t++) {
      foodX = random(SN_COLS); foodY = random(SN_ROWS);
      bool on = false;
      for (int i = 0; i < snLen; i++) if (snX[i] == foodX && snY[i] == foodY) on = true;
      if (!on) break;
    }
  }
}
static void drawSnake() {
  rect(0, 0, 320, 24, C_CARD);
  text(8, 4, "‹ Jogos", F_BS13, C_BLUE, C_CARD);
  text(160, 5, "SNAKE · " + String(snScore), F_CS12, C_INK, C_CARD, 1);
  text(312, 5, "recorde " + String(cfg.snakeBest), F_CS12, C_MUTED, C_CARD, 2);
  rect(0, 24, 320, 2, C_OR);
  rect(0, SN_Y, 320, SN_ROWS * SN_CELL, C_SOFT);
  rrect(foodX * SN_CELL + 3, SN_Y + foodY * SN_CELL + 3, 10, 10, 5, C_OR, C_OR);
  for (int i = snLen - 1; i >= 0; i--)
    rrect(snX[i] * SN_CELL + 1, SN_Y + snY[i] * SN_CELL + 1, 14, 14, 3, i == 0 ? C_INK : C_GREEN, i == 0 ? C_INK : C_GREEN);
  if (snDead) {
    rrect(60, 90, 200, 60, 8, C_INK, C_INK);
    text(160, 98, "Fim de jogo · " + String(snScore), F_BS16, C_WHITE, C_INK, 1);
    text(160, 122, "toque pra jogar de novo", F_BM11, C_WHITE, C_INK, 1);
  }
}

static void drawJogos() {
  header("JOGOS");
  const char* n[4] = {"Snake", "Tanques", "2048", "Pong"};
  const char* d[4] = {"Toque na direção", "Artilharia contra a placa", "em breve", "em breve"};
  for (int i = 0; i < 4; i++) {
    int x = 8 + (i % 2) * 156, y = 32 + (i / 2) * 78;
    bool live_ = i <= 1;
    rrect(x, y, 148, 72, 6, live_ ? C_OR : C_CARD, live_ ? C_OR : C_LINE);
    text(x + 12, y + 12, n[i], F_CB24, live_ ? C_INK : C_MUTED, live_ ? C_OR : C_CARD);
    text(x + 12, y + 46, d[i], F_BM11, live_ ? C_INK : C_MUTED, live_ ? C_OR : C_CARD);
  }
  text(8, 196, "Recorde no Snake: " + String(cfg.snakeBest), F_BS13, C_INK, C_BG);
  text(8, 220, "Recordes ficam salvos na placa", F_BM11, C_MUTED, C_BG);
}


// ---------- tanks: turn-based artillery against the board ----------
static const int TK_TOP = 26, TK_GROUND = 194;      // play field between the top bar and the controls
static uint8_t tkH[320];                            // ground surface (screen y) per column
static int tkX[2], tkHp[2], tkAng[2], tkPow[2], tkWind = 0, tkTurn = 0, tkPhase = 0, tkWinner = -1, tkShots = 0, tkBoomR = 0;
static float tkPx, tkPy, tkVx, tkVy;                // shell
static int tkTrailN = 0; static int16_t tkTrail[90][2];
static int tkBx = 0, tkBy = 0;                      // last explosion
static uint32_t tkTick = 0, tkWait = 0;
enum { TK_AIM, TK_FLY, TK_BOOM, TK_OVER };
static const float TK_K = 0.065f, TK_G = 0.12f;
static int tkY(int i) { return tkH[constrain(tkX[i], 0, 319)]; }
static void tanksNew() {
  float a1 = random(10, 28), a2 = random(5, 16), f1 = random(8, 18) / 1000.0f, f2 = random(25, 45) / 1000.0f, ph1 = random(0, 628) / 100.0f, ph2 = random(0, 628) / 100.0f;
  int base = random(130, 160);
  for (int x = 0; x < 320; x++) tkH[x] = constrain((int)(base + a1 * sinf(x * f1 + ph1) + a2 * sinf(x * f2 + ph2)), 70, 184);
  tkX[0] = random(24, 64); tkX[1] = random(256, 296);
  for (int i = 0; i < 2; i++) {                     // a small flat pad under each tank
    int y = tkH[tkX[i]];
    for (int x = tkX[i] - 9; x <= tkX[i] + 9; x++) tkH[constrain(x, 0, 319)] = y;
    tkHp[i] = 100;
  }
  tkAng[0] = 45; tkAng[1] = 135; tkPow[0] = 60; tkPow[1] = 60;
  tkWind = random(-8, 9); tkTurn = 0; tkPhase = TK_AIM; tkWinner = -1; tkShots = 0; tkTrailN = 0;
}
static void tanksFire() {
  float a = tkAng[tkTurn] * DEG_TO_RAD, v = tkPow[tkTurn] * TK_K;
  tkPx = tkX[tkTurn] + cosf(a) * 11; tkPy = tkY(tkTurn) - 8 - sinf(a) * 11;
  tkVx = cosf(a) * v; tkVy = -sinf(a) * v;
  tkTrailN = 0; tkPhase = TK_FLY; tkShots++;
}
static int tanksSimulate(int ang, int pow, int from) {   // where a shot lands (x), for the board's aim
  float a = ang * DEG_TO_RAD, v = pow * TK_K, x = tkX[from] + cosf(a) * 11, y = tkY(from) - 8 - sinf(a) * 11, vx = cosf(a) * v, vy = -sinf(a) * v;
  for (int i = 0; i < 1500; i++) {
    vx += tkWind * 0.004f; vy += TK_G; x += vx; y += vy;
    if (x < 0 || x >= 320) return (int)x;
    if (y >= tkH[(int)x]) return (int)x;
  }
  return (int)x;
}
static void tanksCpuAim() {
  int target = tkX[0], bestP = 60, bestD = 9999, ang = random(118, 152);
  for (int p = 20; p <= 100; p++) { int d = abs(tanksSimulate(ang, p, 1) - target); if (d < bestD) { bestD = d; bestP = p; } }
  int err = max(2, 12 - tkShots);                   // gets sharper as the match goes on
  tkAng[1] = ang; tkPow[1] = constrain(bestP + (int)random(-err, err + 1), 20, 100);
}
static void tanksExplode(int bx, int by) {
  const int R = 16;
  tkBx = bx; tkBy = by;
  for (int x = bx - R; x <= bx + R; x++) {
    if (x < 0 || x >= 320) continue;
    int dy = (int)sqrtf(R * R - (x - bx) * (x - bx));
    int bottom = min(TK_GROUND - 2, by + dy);
    if (tkH[x] < bottom && tkH[x] > by - dy - 1) tkH[x] = bottom;          // the ground inside the blast goes
    else if (tkH[x] <= by - dy - 1 && by + dy > tkH[x]) tkH[x] = min(TK_GROUND - 2, tkH[x] + 2 * dy);   // blast under the surface
  }
  for (int i = 0; i < 2; i++) {
    float d = sqrtf((float)(tkX[i] - bx) * (tkX[i] - bx) + (float)(tkY(i) - 4 - by) * (tkY(i) - 4 - by));
    if (d < R + 8) tkHp[i] = max(0, tkHp[i] - (int)lroundf(55 * (1 - d / (R + 8))));
  }
  tkBoomR = 1; tkPhase = TK_BOOM;
}
static void tanksStep() {
  if (tkPhase == TK_FLY) {
    for (int k = 0; k < 4; k++) {
      tkVx += tkWind * 0.004f; tkVy += TK_G; tkPx += tkVx; tkPy += tkVy;
      if (tkTrailN < 90 && (k & 1)) { tkTrail[tkTrailN][0] = tkPx; tkTrail[tkTrailN][1] = tkPy; tkTrailN++; }
      if (tkPx < 0 || tkPx >= 320 || tkPy > TK_GROUND) { tkBoomR = 0; tkPhase = TK_BOOM; tkBx = -100; break; }   // out of the field
      bool hitTank = false;
      for (int i = 0; i < 2; i++) if (abs((int)tkPx - tkX[i]) <= 8 && tkPy >= tkY(i) - 9 && tkPy <= tkY(i)) hitTank = true;
      if (tkPy >= tkH[(int)tkPx] || hitTank) { tanksExplode((int)tkPx, (int)tkPy); break; }
    }
  } else if (tkPhase == TK_BOOM) {
    if (tkBoomR && tkBoomR < 16) { tkBoomR += 3; return; }
    tkBoomR = 0; tkTrailN = 0;
    if (tkHp[0] <= 0 || tkHp[1] <= 0) { tkWinner = tkHp[0] <= 0 && tkHp[1] <= 0 ? 2 : tkHp[1] <= 0 ? 0 : 1; tkPhase = TK_OVER; return; }
    tkTurn ^= 1; tkPhase = TK_AIM;
    if (tkTurn == 0) tkWind = constrain(tkWind + (int)random(-3, 4), -10, 10);   // the wind changes a little each round
    else { tanksCpuAim(); tkWait = millis() + 900; }
  } else if (tkPhase == TK_AIM && tkTurn == 1 && millis() > tkWait) tanksFire();
}
static void drawTank(int i) {
  int x = tkX[i], y = tkY(i);
  uint16_t c = i == 0 ? C_GREEN : C_RED;
  float a = tkAng[i] * DEG_TO_RAD;
  int bx = x + (int)lroundf(cosf(a) * 11), by = y - 8 - (int)lroundf(sinf(a) * 11);
  spr.drawWideLine(x, y - 8 - yoff, bx, by - yoff, 2, C_INK);
  rrect(x - 8, y - 7, 16, 7, 2, c, c);
  circle(x, y - 8, 4, c);
}
static void drawTanks() {
  rect(0, 0, 320, 24, C_CARD);
  text(8, 4, "‹ Jogos", F_BS13, C_BLUE, C_CARD);
  text(96, 5, "VOCÊ " + String(tkHp[0]), F_CS12, C_GREEN, C_CARD);
  String w = tkWind == 0 ? String("sem vento") : String(tkWind < 0 ? "‹ vento " : "vento ") + abs(tkWind) + (tkWind > 0 ? " ›" : "");
  text(206, 5, w, F_CS12, C_MUTED, C_CARD, 1);
  text(312, 5, "CPU " + String(tkHp[1]), F_CS12, C_RED, C_CARD, 2);
  rect(0, 24, 320, 2, C_OR);
  rect(0, TK_TOP, 320, TK_GROUND - TK_TOP, C_BG);
  for (int x = 0; x < 320; x++) spr.drawFastVLine(x, tkH[x] - yoff, TK_GROUND - tkH[x], C_SOFT == C_BG ? C_LINE : C_SOFT);
  for (int x = 0; x < 320; x++) spr.drawPixel(x, tkH[x] - yoff, C_MUTED);
  for (int i = 0; i < 2; i++) if (tkHp[i] > 0 || tkPhase != TK_OVER) drawTank(i);
  for (int i = 0; i < tkTrailN; i++) spr.drawPixel(tkTrail[i][0], tkTrail[i][1] - yoff, C_MUTED);
  if (tkPhase == TK_FLY) circle((int)tkPx, (int)tkPy, 2, C_INK);
  if (tkPhase == TK_BOOM && tkBoomR) { circle(tkBx, tkBy, tkBoomR, C_OR); if (tkBoomR > 6) circle(tkBx, tkBy, tkBoomR - 6, C_WARN); }
  // controls
  rect(0, TK_GROUND, 320, 240 - TK_GROUND, C_CARD);
  hline(0, TK_GROUND, 320, C_LINE);
  if (tkPhase == TK_OVER) {
    const char* m = tkWinner == 0 ? "Você venceu!" : tkWinner == 1 ? "A placa venceu" : "Empate";
    text(160, 202, m, F_BS16, C_INK, C_CARD, 1);
    text(160, 222, "toque pra jogar de novo", F_BM11, C_MUTED, C_CARD, 1);
    return;
  }
  bool mine = tkTurn == 0 && tkPhase == TK_AIM;
  uint16_t k = mine ? C_INK : C_MUTED;
  const int Y = TK_GROUND + 6, H = 34;
  rrect(4, Y, 30, H, 5, C_BG, C_LINE);   text(19, Y + 9, "‹", F_BS16, k, C_BG, 1);
  text(64, Y + 3, "ângulo", F_BM11, C_MUTED, C_CARD, 1); text(64, Y + 16, String(tkAng[0]) + "°", F_BS16, k, C_CARD, 1);
  rrect(94, Y, 30, H, 5, C_BG, C_LINE);  text(109, Y + 9, "›", F_BS16, k, C_BG, 1);
  rrect(130, Y, 30, H, 5, C_BG, C_LINE); text(145, Y + 8, "−", F_BS16, k, C_BG, 1);
  text(190, Y + 3, "força", F_BM11, C_MUTED, C_CARD, 1); text(190, Y + 16, String(tkPow[0]), F_BS16, k, C_CARD, 1);
  rrect(220, Y, 30, H, 5, C_BG, C_LINE); text(235, Y + 8, "+", F_BS16, k, C_BG, 1);
  rrect(256, Y, 60, H, 6, mine ? C_RED : C_LINE, mine ? C_RED : C_LINE);
  text(286, Y + 9, tkTurn == 1 ? "CPU…" : "FOGO", F_BS13, C_WHITE, mine ? C_RED : C_LINE, 1);
}
static void tanksTap(int x, int y) {
  if (y < 26 && x < 90) { uiGoto(S_JOGOS); return; }
  if (tkPhase == TK_OVER) { tanksNew(); redraw = true; return; }
  if (tkTurn != 0 || tkPhase != TK_AIM) return;
  if (y >= TK_GROUND) {
    if (x < 40) tkAng[0] = min(170, tkAng[0] + 2);          // ‹ turns the barrel left (up/back)
    else if (x >= 88 && x < 126) tkAng[0] = max(10, tkAng[0] - 2);
    else if (x >= 126 && x < 164) tkPow[0] = max(20, tkPow[0] - 2);
    else if (x >= 214 && x < 252) tkPow[0] = min(100, tkPow[0] + 2);
    else if (x >= 252) tanksFire();
  } else if (y > TK_TOP) {                                  // tap the sky: aim there (direction and strength)
    int dx = x - tkX[0], dy = (tkY(0) - 8) - y;
    if (dy > 2) {
      tkAng[0] = constrain((int)lroundf(atan2f(dy, dx) * RAD_TO_DEG), 10, 170);
      tkPow[0] = constrain((int)(sqrtf(dx * dx + dy * dy) / 2.2f), 20, 100);
    }
  }
  redraw = true;
}


// ---------- SSH terminal ----------
static const int TM_Y = 14, TM_LH = 12, TM_CW = 6;   // top bar, line height, cell width (IBM Plex Mono 10 px)
static bool tmKb = true;                              // touch keyboard shown
static bool tmCtrl = false; static int tmMode = 0;    // 0 abc, 1 ABC (one-shot), 2 123
static const char* TK_ROWS[3][3] = {{"qwertyuiop", "asdfghjkl", "zxcvbnm"}, {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"}, {"1234567890", "-/:;()$&@\"", ".,?!'|~=+"}};
static const int TKB_Y = 112, TKB_H = 23, TKB_G = 2;  // 5 rows of 23 px from y=112
static uint32_t tmVersion = 0, tmLastDraw = 0;
static uint16_t ansi(uint8_t c, bool fg) {
  if (c == 255) return fg ? C_INK : C_BG;
  bool dark = cfg.theme == 1;
  static const uint32_t L[16] = {0x16191D, 0xB3261E, 0x1E7A3A, 0x8A6A12, 0x2D5FA8, 0x8B3FA0, 0x0B6E78, 0x5B6570,
                                 0x6B7280, 0xD93025, 0x2B9348, 0xB7791F, 0x3B82F6, 0xA855F7, 0x0E9AA7, 0x16191D};
  static const uint32_t D[16] = {0x2A2F2B, 0xFF6B5E, 0x2FD675, 0xFFB020, 0x6CA8FF, 0xD08CFF, 0x4FD8E0, 0xC9D0CA,
                                 0x7D8A82, 0xFF8A80, 0x7CF0A8, 0xFFD166, 0x9CC5FF, 0xE3B5FF, 0x8EEBF0, 0xFFFFFF};
  uint32_t v = (dark ? D : L)[c & 15];
  return lcd.color565((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}
static void tmKey(int x, int y, int w, const String& lab, bool dark) {
  rrect(x, y, w, TKB_H, 4, dark ? C_INK : C_CARD, dark ? C_INK : C_LINE);
  text(x + w / 2, y + 5, lab, F_BM11, dark ? C_WHITE : C_INK, dark ? C_INK : C_CARD, 1);
}
static int tmRowX(int n, int kw) { return (320 - (n * kw + (n - 1) * TKB_G)) / 2; }
static void drawSsh() {
  // top bar: leave, who, keyboard toggle
  rect(0, 0, 320, TM_Y, C_CARD);
  text(4, 1, "‹ sair", F_BM11, C_BLUE, C_CARD);
  String who = sshState == 2 ? sshMsg : sshState == 1 ? String("conectando…") : String("terminal");
  text(160, 1, who, F_BM11, sshState == 2 ? C_GREEN : C_MUTED, C_CARD, 1);
  text(316, 1, tmKb ? "esconder teclado" : "teclado", F_BM11, C_BLUE, C_CARD, 2);
  int rows = tmKb ? (TKB_Y - TM_Y) / TM_LH : VT_ROWS;
  int first = tmKb ? constrain(vt.cy - rows + 2, 0, VT_ROWS - rows) : 0;
  rect(0, TM_Y, 320, rows * TM_LH + 2, C_BG);
  {
    font(T_BM11);
    for (int r = 0; r < rows; r++) {
      int vr = first + r, y = TM_Y + 1 + r * TM_LH;
      int c = 0;
      while (c < VT_COLS) {             // one run per stretch of equal colours
        const VtCell& a = vt.cell[vr][c];
        int c2 = c; String run;
        while (c2 < VT_COLS && vt.cell[vr][c2].fg == a.fg && vt.cell[vr][c2].bg == a.bg && vt.cell[vr][c2].attr == a.attr) {
          uint16_t ch = vt.cell[vr][c2].ch;
          if (ch < 0x80) run += (char)ch; else if (ch < 0x800) { run += (char)(0xC0 | (ch >> 6)); run += (char)(0x80 | (ch & 0x3F)); }
          else { run += (char)(0xE0 | (ch >> 12)); run += (char)(0x80 | ((ch >> 6) & 0x3F)); run += (char)(0x80 | (ch & 0x3F)); }
          c2++;
        }
        bool inv = a.attr & VT_INVERSE;
        uint16_t fg = ansi(a.fg, true), bg = ansi(a.bg, false);
        if (a.attr & VT_BOLD && a.fg < 8 && a.fg != 255) fg = ansi(a.fg + 8, true);
        if (inv) { uint16_t t = fg; fg = bg; bg = t; }
        if (bg != C_BG) rect(c * TM_CW, y, (c2 - c) * TM_CW, TM_LH, bg);
        int lead = 0; while (lead < (int)run.length() && run[lead] == ' ') lead++;   // spaces are just background
        int end = run.length(); while (end > lead && run[end - 1] == ' ') end--;
        if (end > lead) {
          spr.setTextColor(fg, bg);
          spr.setTextDatum(textdatum_t::top_left);
          spr.drawString(run.substring(lead, end), (c + lead) * TM_CW, y - yoff);   // ASCII before any accented char keeps the column count
        }
        c = c2;
      }
      if (vt.cursorOn && vr == vt.cy) rect(vt.cx * TM_CW, y + TM_LH - 3, TM_CW, 2, C_GREEN);
    }
  }
  if (!tmKb) return;
  // keyboard: special row, 3 letter rows, bottom row
  rect(0, TKB_Y - 2, 320, 240 - TKB_Y + 2, C_SOFT == C_BG ? C_CARD : C_SOFT);
  const char* sp[8] = {"esc", "tab", "ctrl", "‹", "v", "^", "›", "del"};
  for (int i = 0; i < 8; i++) tmKey(2 + i * 40, TKB_Y, 36, sp[i], (i == 2 && tmCtrl));
  const char** rows3 = TK_ROWS[tmMode];
  for (int r = 0; r < 3; r++) {
    int n = strlen(rows3[r]), kw = 30, x0 = tmRowX(n, kw);
    if (r == 2) x0 = tmRowX(n, kw);
    for (int i = 0; i < n; i++) tmKey(x0 + i * (kw + TKB_G), TKB_Y + (r + 1) * (TKB_H + TKB_G), kw, String(rows3[r][i]), false);
  }
  int yb = TKB_Y + 4 * (TKB_H + TKB_G);
  tmKey(2, yb, 46, tmMode == 2 ? "abc" : "123", false);
  tmKey(50, yb, 46, tmMode == 1 ? "ABC" : "shift", tmMode == 1);
  tmKey(98, yb, 150, "espaço", false);
  tmKey(250, yb, 68, "enter", true);
}
static void tmSendKey(const char* s) { termInput(s, strlen(s)); }
static void sshTap(int x, int y) {
  if (y < TM_Y + 4) {
    if (x < 60) { sshStop(); uiGoto(S_APPS); return; }
    if (x > 250) { tmKb = !tmKb; redraw = true; return; }
    return;
  }
  if (!tmKb || y < TKB_Y) return;
  int row = (y - TKB_Y) / (TKB_H + TKB_G);
  if (row == 0) {
    int i = constrain(x / 40, 0, 7);
    const char* arrows[4] = {vt.appCursor ? "\x1bOD" : "\x1b[D", vt.appCursor ? "\x1bOB" : "\x1b[B", vt.appCursor ? "\x1bOA" : "\x1b[A", vt.appCursor ? "\x1bOC" : "\x1b[C"};
    if (i == 0) tmSendKey("\x1b"); else if (i == 1) tmSendKey("\t"); else if (i == 2) { tmCtrl = !tmCtrl; redraw = true; }
    else if (i <= 6) tmSendKey(arrows[i - 3]); else tmSendKey("\x7f");
    return;
  }
  if (row >= 1 && row <= 3) {
    const char* r = TK_ROWS[tmMode][row - 1];
    int n = strlen(r), kw = 30, x0 = tmRowX(n, kw);
    int i = (x - x0 + TKB_G / 2) / (kw + TKB_G);
    if (x < x0 - TKB_G || i < 0 || i >= n) return;
    char c = r[i];
    if (tmCtrl && isalpha((unsigned char)c)) { c = toupper(c) & 0x1F; tmCtrl = false; redraw = true; }   // ctrl+c, ctrl+d...
    termInput(&c, 1);
    if (tmMode == 1) { tmMode = 0; redraw = true; }
    return;
  }
  if (x < 48) tmMode = tmMode == 2 ? 0 : 2;
  else if (x < 96) tmMode = tmMode == 1 ? 0 : 1;
  else if (x < 248) tmSendKey(" ");
  else tmSendKey("\r");
  redraw = true;
}

static int irDev = 0;
static void drawIr() {
  header("CONTROLE REMOTO");
  const char* devs[3] = {"TV da sala", "Ar", "Som"};
  for (int i = 0; i < 3; i++) {
    int x = 8 + i * 102;
    bool on = i == irDev;
    rrect(x, 32, 96, 28, 5, on ? C_INK : C_CARD, on ? C_INK : C_LINE);
    text(x + 48, 38, devs[i], F_BS13, on ? C_WHITE : C_INK, on ? C_INK : C_CARD, 1);
  }
  const char* keys[8] = {"Liga", "Vol +", "Canal +", "Mudo", "Fonte", "Vol −", "Canal −", "OK"};
  for (int i = 0; i < 8; i++) {
    int x = 8 + (i % 4) * 77, y = 66 + (i / 4) * 54;
    rrect(x, y, 71, 48, 6, i == 0 ? C_OR : C_CARD, i == 0 ? C_OR : C_LINE);
    text(x + 35, y + 16, keys[i], F_BS13, C_INK, i == 0 ? C_OR : C_CARD, 1);
  }
  card(8, 178, 304, 30);
  text(18, 185, "LED IR não instalado ainda", F_BS13, C_ORT, C_CARD);
  text(8, 220, "Ligue um LED IR no conector CN1 (GPIO27) pra ativar", F_BM11, C_MUTED, C_BG);
}

static String ago(uint32_t t) {
  time_t now; time(&now);
  if (!t || t > (uint32_t)now + 60) return "";
  long s = (long)now - (long)t;
  if (s < 60) return "agora";
  if (s < 3600) return String(s / 60) + " min";
  if (s < 86400) return String(s / 3600) + " h";
  return String(s / 86400) + " d";
}
static void drawMeshMsgs(int y, int maxN) {
  for (int i = 0; i < live.dkMsgN && i < maxN; i++) {
    int yy = y + i * 34;
    if (i) hline(9, yy - 3, 302, C_SOFT);
    text(18, yy, live.dkMsgDe[i], F_BS13, C_ORT, C_CARD);
    text(302, yy, ago(live.dkMsgT[i]), F_BM11, C_MUTED, C_CARD, 2);
    String m = live.dkMsgTxt[i];
    text(18, yy + 16, m, fit(m, 284, F_BM13, F_BM11), C_INK, C_CARD);
  }
}
static void drawMesh() {
  header("MESH · MESHTASTIC");
  if (deckFresh() && live.dkMeshN >= 0) {
    rrect(8, 30, 304, 44, 6, C_INK, C_INK);
    text(18, 34, String(live.dkMeshN), F_CB34, C_WHITE, C_INK);
    text(18 + tw(String(live.dkMeshN), F_CB34) + 8, 40, live.dkMeshN == 1 ? "nó na malha" : "nós na malha", F_BS13, C_PEACH, C_INK);
    text(302, 44, "via " + live.dkHost, F_BM11, C_OR, C_INK, 2);
    card(8, 80, 304, 136);
    if (!live.dkMsgN) text(18, 92, "Nenhuma mensagem ainda", F_BS13, C_MUTED, C_CARD);
    drawMeshMsgs(88, 3);
    text(8, 222, "Rádio LoRa ligado no deck", F_BM11, C_MUTED, C_BG);
    return;
  }
  card(8, 32, 304, 120);
  if (!cfg.meshIp.length()) {
    text(18, 42, "Nó base não configurado", F_BS16, C_INK, C_CARD);
    wrap(18, 66, 284, "Coloque o IP do seu nó Meshtastic em Config › Pelo celular. A CYD conversa com ele pela rede.", F_BM13, C_MUTED, C_CARD, 17, 4);
  } else {
    text(18, 42, "Nó base: " + cfg.meshIp, F_BS16, C_INK, C_CARD);
    wrap(18, 66, 284, "A lista de nós e as mensagens chegam na próxima versão do firmware.", F_BM13, C_MUTED, C_CARD, 17, 4);
  }
  text(8, 220, "Lista de nós e mensagens: próxima versão", F_BM11, C_MUTED, C_BG);
}

static void plFoot() {
  String f = live.plOk ? "feed · atualizado " + hhmm(live.plAt) : (live.plErr.length() ? live.plErr : String("aguardando o feed"));
  text(8, 222, f, F_BM11, live.plOk ? C_MUTED : C_ORT, C_BG);
}
static void agFoot() {
  if (!cfg.icalUrl.length()) { plFoot(); return; }
  String f = live.icErr.length() ? live.icErr : live.icOk ? "agenda iCal · atualizada " + hhmm(live.icAt) : String("lendo a agenda…");
  text(8, 222, f, F_BM11, live.icErr.length() ? C_ORT : C_MUTED, C_BG);
}
static void drawAgenda() {
  header("AGENDA");
  if (!live.agN) {
    card(8, 32, 304, 90);
    bool ok = cfg.icalUrl.length() ? live.icOk : live.plOk;
    text(18, 42, ok ? "Nada nos próximos dias" : "Agenda ainda não chegou", F_BS16, C_INK, C_CARD);
    wrap(18, 66, 284, ok ? (cfg.icalUrl.length() ? "Nada marcado nas próximas duas semanas." : "Os compromissos vêm do feed do seu servidor.")
                         : "Cole o link iCal da sua agenda (Google: Endereço secreto no formato iCal) em Config › Pelo celular.", F_BM13, C_MUTED, C_CARD, 17, 3);
    agFoot();
    return;
  }
  for (int i = 0; i < live.agN; i++) {
    int y = 32 + i * 60;
    bool first = i == 0;
    rrect(8, y, 304, 54, 6, first ? C_INK : C_CARD, first ? C_INK : C_LINE);
    uint16_t bg = first ? C_INK : C_CARD;
    String t = live.agT[i];
    text(18, y + 8, t, fit(t, 200, F_BS16, F_BS13, F_BM11), first ? C_WHITE : C_INK, bg);
    text(18, y + 31, whenLabel(live.agQ[i], live.agDia[i]), F_BM13, first ? C_PEACH : C_MUTED, bg);
    text(302, y + 10, countdown(live.agQ[i]), F_BS13, first ? C_OR : C_ORT, bg, 2);
  }
  agFoot();
}
static void drawNews() {
  header("MANCHETES");
  if (!live.nwN) {
    card(8, 32, 304, 80);
    text(18, 42, "Sem manchetes ainda", F_BS16, C_INK, C_CARD);
    wrap(18, 66, 284, "As manchetes vêm do feed do seu servidor.", F_BM13, C_MUTED, C_CARD, 17, 2);
    plFoot();
    return;
  }
  card(8, 30, 304, 186);
  for (int i = 0; i < live.nwN; i++) {
    int y = 30 + i * 37;
    if (i) hline(9, y, 302, C_SOFT);
    text(18, y + 6, String(i + 1), F_CB18, C_OR, C_CARD);
    wrap(40, y + 4, 264, live.nwT[i], F_BM13, C_INK, C_CARD, 15, 2);
  }
  plFoot();
}
static void drawMaq() {
  header("MÁQUINAS");
  card(8, 30, 304, 22 + max(1, live.mqN) * 21);
  text(18, 35, "MÁQUINA", F_CS12, C_MUTED, C_CARD);
  text(170, 35, "CPU", F_CS12, C_MUTED, C_CARD, 2);
  text(214, 35, "RAM", F_CS12, C_MUTED, C_CARD, 2);
  text(262, 35, "DISCO", F_CS12, C_MUTED, C_CARD, 2);
  text(302, 35, "TEMP", F_CS12, C_MUTED, C_CARD, 2);
  if (!live.mqN) text(18, 58, live.plErr.length() ? live.plErr : String("aguardando dados"), F_BM13, C_MUTED, C_CARD);
  for (int i = 0; i < live.mqN; i++) {
    int y = 50 + i * 21;
    hline(9, y, 302, C_SOFT);
    bool on = live.mqOn[i];
    circle(22, y + 11, 4, on ? C_GREEN : C_RED);
    text(32, y + 4, live.mqNome[i], F_BS13, on ? C_INK : C_MUTED, C_CARD);
    if (!on) { text(302, y + 4, "sem sinal há " + agoShort(live.mqAge[i]), F_BS13, C_RED, C_CARD, 2); continue; }
    auto v = [](int x, const char* u) { return x >= 0 ? String(x) + u : String("—"); };
    text(170, y + 4, v(live.mqCpu[i], "%"), F_BS13, live.mqCpu[i] >= 90 ? C_RED : C_INK, C_CARD, 2);
    text(214, y + 4, v(live.mqRam[i], "%"), F_BS13, live.mqRam[i] >= 92 ? C_RED : C_INK, C_CARD, 2);
    text(262, y + 4, v(live.mqDisk[i], "%"), F_BS13, live.mqDisk[i] >= 95 ? C_RED : live.mqDisk[i] >= 85 ? C_ORT : C_INK, C_CARD, 2);
    text(302, y + 4, v(live.mqTemp[i], "°"), F_BS13, live.mqTemp[i] >= 80 ? C_RED : C_INK, C_CARD, 2);
  }
  int y = 30 + 22 + max(1, live.mqN) * 21 + 6;
  if (y + 62 <= 220) {
    rrect(8, y, 304, 62, 6, C_INK, C_INK);
    text(18, y + 6, "USO DE IA · 30 DIAS", F_CS12, C_OR, C_INK);
    text(18, y + 22, live.iaCusto >= 0 ? "US$ " + fmtNum(live.iaCusto, 2) : String("—"), F_CB24, C_WHITE, C_INK);
    String tk = live.iaTokens >= 1e9 ? fmtNum(live.iaTokens / 1e9, 2) + " bi tokens" : live.iaTokens >= 1e6 ? fmtNum(live.iaTokens / 1e6, 1) + " mi tokens"
              : live.iaTokens >= 0 ? fmtNum(live.iaTokens, 0) + " tokens" : String("");
    text(302, y + 8, tk, F_BM11, C_WHITE, C_INK, 2);
    String pm;
    for (int i = 0; i < live.iaN && i < 3; i++) pm += (i ? " · " : "") + live.iaNome[i] + " $" + fmtNum(live.iaC[i], 0);
    text(302, y + 26, pm, fit(pm, 150, F_BM11, F_BM11), C_PEACH, C_INK, 2);
    text(302, y + 44, "preço de API, não a fatura", F_BM11, C_MUTED, C_INK, 2);
  }
  plFoot();
}
static String fmtUp(uint32_t s) {
  if (s >= 86400) return String(s / 86400) + " d " + String((s % 86400) / 3600) + " h";
  if (s >= 3600) return String(s / 3600) + " h " + String((s % 3600) / 60) + " min";
  return String(s / 60) + " min";
}
static void drawDeck() {
  header("DECK");
  if (!deckFresh()) {
    card(8, 32, 304, 110);
    text(18, 42, live.dkAt ? "O deck parou de responder" : "Deck não conectado", F_BS16, C_INK, C_CARD);
    wrap(18, 66, 284, "Ligue a placa no USB do Orange Pi / Raspberry Pi e rode o deckd (pasta host/ do projeto).", F_BM13, C_MUTED, C_CARD, 17, 4);
    return;
  }
  // battery
  bool low = live.dkBat >= 0 && live.dkBat <= 15 && !live.dkChg;
  uint16_t bbg = low ? C_RED : C_INK;
  rrect(8, 30, 148, 74, 6, bbg, bbg);
  text(18, 34, "BATERIA", F_CS12, C_OR, bbg);
  text(18, 48, live.dkBat >= 0 ? String(live.dkBat) + "%" : String("—"), F_CB34, C_WHITE, bbg);
  if (live.dkBat >= 0) {
    rrect(18, 86, 128, 6, 3, C_MUTED, C_MUTED);
    rrect(18, 86, max(6, 128 * live.dkBat / 100), 6, 3, low ? C_WHITE : C_OR, low ? C_WHITE : C_OR);
  }
  String bl = live.dkChg ? "carregando" : (live.dkV > 0 ? fmtNum(live.dkV, 2) + " V" : String(""));
  text(146, 36, bl, F_BM11, C_PEACH, bbg, 2);
  // system
  card(164, 30, 148, 74);
  text(174, 34, String(live.dkHost).substring(0, 16), F_CS12, C_MUTED, C_CARD);
  const char* lab[3] = {"CPU", "RAM", "TEMP"};
  int val[3] = {live.dkCpu, live.dkRam, live.dkTemp};
  const char* un[3] = {"%", "%", "°"};
  int lim[3] = {90, 90, 75};
  for (int i = 0; i < 3; i++) {
    int x = 174 + i * 46;
    text(x, 50, lab[i], F_BM11, C_MUTED, C_CARD);
    text(x, 64, val[i] >= 0 ? String(val[i]) + un[i] : String("—"), F_BS16, val[i] >= lim[i] ? C_RED : C_INK, C_CARD);
  }
  text(174, 86, "ligado há " + fmtUp(live.dkUp), F_BM11, C_MUTED, C_CARD);
  // networks
  card(8, 110, 304, 44);
  String w = live.dkWifi.length() ? "Wi-Fi " + live.dkWifi + (live.dkIp.length() ? " · " + live.dkIp : String("")) : String("Wi-Fi desligado");
  text(18, 115, w, fit(w, 284, F_BS13, F_BM11), live.dkWifi.length() ? C_INK : C_MUTED, C_CARD);
  String l = live.dkLte.length() ? "4G " + live.dkLte + (live.dkSig >= 0 ? " · " + String(live.dkSig) + "%" : String("")) : String("4G —");
  String g = live.dkSats >= 0 ? (live.dkFix ? "GPS ok · " : "GPS sem fix · ") + String(live.dkSats) + " sat" : String("GPS —");
  text(18, 134, l, F_BM11, live.dkLte.length() ? C_INK : C_MUTED, C_CARD);
  text(302, 134, g, F_BM11, live.dkFix ? C_GREEN : C_MUTED, C_CARD, 2);
  // mesh
  card(8, 160, 304, 56);
  text(18, 164, live.dkMeshN >= 0 ? "MESH · " + String(live.dkMeshN) + " nós" : String("MESH · sem rádio"), F_CS12, C_ORT, C_CARD);
  if (live.dkMsgN) {
    String m = live.dkMsgDe[0] + ": " + live.dkMsgTxt[0];
    text(302, 164, ago(live.dkMsgT[0]), F_BM11, C_MUTED, C_CARD, 2);
    wrap(18, 180, 284, m, F_BM13, C_INK, C_CARD, 16, 2);
  } else text(18, 184, live.dkMeshN >= 0 ? "Nenhuma mensagem ainda" : "Ligue um Meshtastic no USB do deck", F_BM13, C_MUTED, C_CARD);
  String foot = live.dkAlert.length() ? live.dkAlert : "conectado por USB · " + String((millis() - live.dkAt) / 1000) + " s";
  text(8, 222, foot, F_BM11, live.dkAlert.length() ? C_RED : C_MUTED, C_BG);
}

static int appsList(int* out) {
  const int sc[12] = {S_AGENDA, S_NEWS, S_MAQ, S_BTC, S_CLIMA, S_REDES, S_PIHOLE, S_CASA, S_AGENTE, S_JOGOS, S_IR, S_MESH};
  const int md[12] = {M_AGENDA, M_NEWS, M_MAQ, M_BTC, M_CLIMA, M_REDES, M_PIHOLE, M_CASA, M_AGENTE, M_JOGOS, M_IR, M_MESH};
  int n = 0;
  for (int i = 0; i < 12; i++) if (MOD_ON(md[i])) out[n++] = sc[i];
  out[n++] = S_RADIO; out[n++] = S_SSH; out[n++] = S_APAGAO;   // always there (Avisos: tap the top bar)
  return n;
}
static const int APP_PITCH = 41, APP_H = 37;
static void drawApps() {
  header("TODOS OS APPS");
  int a[15]; int n = appsList(a);
  for (int i = 0; i < n; i++) {
    int x = 8 + (i % 3) * 102, y = 29 + (i / 3) * APP_PITCH;
    rrect(x, y, 96, APP_H, 6, C_CARD, C_LINE);
    String l = dockLabel(a[i]);
    text(x + 48, y + 10, l, fit(l, 88, F_BS13, F_BM11), C_INK, C_CARD, 1);
  }
}

static void drawPihole() {
  header("PI-HOLE");
  if (!cfg.phUrl.length()) {
    card(8, 32, 304, 100);
    text(18, 42, "Pi-hole não configurado", F_BS16, C_INK, C_CARD);
    wrap(18, 66, 284, "Coloque o endereço do Pi-hole (ex: http://192.168.1.50) e a senha em Config › Pelo celular.", F_BM13, C_MUTED, C_CARD, 17, 3);
    return;
  }
  rrect(8, 32, 304, 60, 6, C_INK, C_INK);
  text(18, 38, live.phOk ? fmtNum(live.phPct, 1) + "%" : "—", F_CB34, C_WHITE, C_INK);
  text(170, 44, "das consultas", F_BM11, C_WHITE, C_INK);
  text(170, 59, "bloqueadas hoje", F_BM11, C_WHITE, C_INK);
  if (live.phOk) {
    rrect(18, 80, 284, 6, 3, C_MUTED, C_MUTED);
    rrect(18, 80, max(6, (int)(284 * live.phPct / 100)), 6, 3, C_OR, C_OR);
  }
  const char* lab[4] = {"Consultas hoje", "Bloqueadas", "Clientes ativos", "Domínios na lista"};
  long val[4] = {live.phTotal, live.phBlocked, (long)live.phClients, live.phDomains};
  for (int i = 0; i < 4; i++) {
    int x = 8 + (i % 2) * 156, y = 100 + (i / 2) * 56;
    card(x, y, 148, 50);
    text(x + 10, y + 6, lab[i], F_BM11, C_MUTED, C_CARD);
    text(x + 10, y + 21, live.phOk && val[i] >= 0 ? fmtNum(val[i], 0) : "—", F_CB18, C_INK, C_CARD);
  }
  String foot = live.phOk ? "Rede local · atualizado " + hhmm(live.phAt) : (live.phErr.length() ? live.phErr : "carregando…");
  text(8, 222, foot, F_BM11, live.phOk ? C_MUTED : C_ORT, C_BG);
}

static const char* MOD_NAMES[M_COUNT] = {"Bitcoin", "Clima", "Seguidores", "Casa", "Agente", "Jogos", "Controle IR", "Mesh", "Pi-hole", "Agenda", "Notícias", "Máquinas"};
static void drawMods() {
  rect(0, 0, 320, 22, C_CARD);
  rect(0, 22, 320, 2, C_OR);
  text(8, 4, "‹ Config", F_BS13, C_BLUE, C_CARD);
  text(80, 5, "MÓDULOS", F_CS12, C_INK, C_CARD);
  for (int i = 0; i < M_COUNT; i++) {
    int x = 8 + (i % 3) * 102, y = 30 + (i / 3) * 46;
    bool on = (cfg.mods >> i) & 1;
    rrect(x, y, 96, 42, 6, on ? C_INK : C_CARD, on ? C_INK : C_LINE);
    text(x + 48, y + 5, MOD_NAMES[i], fit(MOD_NAMES[i], 88, F_BS13, F_BM11), on ? C_WHITE : C_INK, on ? C_INK : C_CARD, 1);
    text(x + 48, y + 24, on ? "ligado" : "desligado", F_BM11, on ? C_OR : C_MUTED, on ? C_INK : C_CARD, 1);
  }
  text(8, 222, "Toque pra ligar ou desligar. Fica salvo na placa.", F_BM11, C_MUTED, C_BG);
}

static void drawConfig() {
  header("CONFIG");
  card(8, 30, 304, 150);
  // row 1 wifi
  text(18, 37, "Wi-Fi", F_BS13, C_INK, C_CARD);
  String w = wifiUp ? (WiFi.SSID() + " · " + ipStr) : apMode ? "modo configuração" : (savedCount() ? "procurando rede" : "toque pra escolher a rede");
  text(290, 38, w, fit(w, 200, F_BM11, F_BM11), wifiUp ? C_GREEN : C_ORT, C_CARD, 2);
  text(302, 35, "›", F_BS16, C_BLUE, C_CARD, 2);
  hline(9, 58, 302, C_SOFT);
  // row 2 brightness
  text(18, 66, "Brilho", F_BS13, C_INK, C_CARD);
  rrect(84, 62, 30, 24, 4, C_CARD, C_LINE); text(99, 64, "−", F_BS16, C_INK, C_CARD, 1);
  rrect(122, 70, 120, 8, 4, C_SOFT, C_SOFT);
  rrect(122, 70, max(8, 120 * cfg.bright / 255), 8, 4, C_OR, C_OR);
  rrect(250, 62, 30, 24, 4, C_CARD, C_LINE); text(265, 64, "+", F_BS16, C_INK, C_CARD, 1);
  text(302, 66, String(cfg.bright * 100 / 255), F_BS13, C_INK, C_CARD, 2);
  hline(9, 92, 302, C_SOFT);
  // row 3 colors
  text(18, 100, "Tema", F_BS13, C_INK, C_CARD);
  static const char* TN[3] = {"Claro", "Terminal", "Term. claro"};
  rrect(84, 96, 100, 24, 4, C_INK, C_INK);
  text(134, 100, String(TN[cfg.theme % 3]) + " ›", F_BM11, C_WHITE, C_INK, 1);
  rrect(190, 96, 60, 24, 4, cfg.invert ? C_INK : C_CARD, cfg.invert ? C_INK : C_LINE);
  text(220, 100, "Inverter", F_BM11, cfg.invert ? C_WHITE : C_INK, cfg.invert ? C_INK : C_CARD, 1);
  rrect(256, 96, 46, 24, 4, cfg.rgbOrder ? C_INK : C_CARD, cfg.rgbOrder ? C_INK : C_LINE);
  text(279, 100, "R/B", F_BM11, cfg.rgbOrder ? C_WHITE : C_INK, cfg.rgbOrder ? C_INK : C_CARD, 1);
  hline(9, 126, 302, C_SOFT);
  // row 4 touch + restart
  const char* bl[4] = {"Calibrar", "Módulos", "Início", "Reiniciar"};
  for (int i = 0; i < 4; i++) {
    int bx = 16 + i * 72;
    bool hl = i == 1 || i == 2;
    rrect(bx, 134, 68, 34, 5, hl ? C_PEACH : C_CARD, hl ? C_OR : C_LINE);
    text(bx + 34, 143, bl[i], fit(bl[i], 62, F_BS13, F_BM11), C_INK, hl ? C_PEACH : C_CARD, 1);
  }
  rrect(8, 186, 304, 32, 6, C_INK, C_INK);
  text(160, 194, "Configurar pelo celular", F_BS13, C_WHITE, C_INK, 1);
  text(8, 224, "v" FW_VERSION " · tudo salvo na placa", F_BM11, C_MUTED, C_BG);
}

static void drawQr() {
  rect(0, 0, 320, 22, C_CARD);
  rect(0, 22, 320, 2, C_OR);
  text(8, 4, "‹ Config", F_BS13, C_BLUE, C_CARD);
  text(80, 5, "PELO CELULAR", F_CS12, C_INK, C_CARD);
  card(8, 30, 128, 128);
  String qr = apMode ? ("WIFI:T:WPA;S:" AP_SSID ";P:" + cfg.apPass + ";;") : ("http://" + ipStr + "/");
  spr.qrcode(qr.c_str(), 16, 38 - yoff, 112, 4);
  int x = 146;
  if (apMode || !wifiUp) {
    text(x, 32, "1  Conecte na rede", F_BS13, C_INK, C_BG);
    text(x + 16, 49, AP_SSID, F_BS16, C_ORT, C_BG);
    text(x, 72, "   senha " + cfg.apPass, F_BS13, C_INK, C_BG);
    text(x, 94, "2  Abra no navegador", F_BS13, C_INK, C_BG);
    text(x + 16, 111, "192.168.4.1", F_BS16, C_ORT, C_BG);
    text(x, 136, "3  Preencha e salve", F_BS13, C_INK, C_BG);
    text(8, 166, "O QR conecta o celular na rede da placa.", F_BM11, C_MUTED, C_BG);
  } else {
    text(x, 34, "Abra no celular:", F_BS13, C_INK, C_BG);
    text(x, 52, "http://" + ipStr, fit("http://" + ipStr, 170, F_BS16, F_BS13), C_ORT, C_BG);
    text(x, 82, "Senha do painel:", F_BS13, C_INK, C_BG);
    text(x, 100, cfg.apPass, F_CB24, C_INK, C_BG);
    text(8, 166, "O celular precisa estar na mesma rede Wi-Fi.", F_BM11, C_MUTED, C_BG);
  }
  text(8, 184, "Wi-Fi, feed, Home Assistant, agente e", F_BM11, C_MUTED, C_BG);
  text(8, 199, "mesh: tudo fica salvo só na placa.", F_BM11, C_MUTED, C_BG);
}

// ---------- Wi-Fi on the device: saved networks, scan, keyboard, replace when full ----------
static String* const SSIDS[3] = {&cfg.ssid, &cfg.ssid2, &cfg.ssid3};
static String* const PASSES[3] = {&cfg.wpass, &cfg.wpass2, &cfg.wpass3};
static int savedSlot(const String& ssid) { for (int k = 0; k < 3; k++) if (*SSIDS[k] == ssid) return k; return -1; }
static int freeSlot() { for (int k = 0; k < 3; k++) if (!SSIDS[k]->length()) return k; return -1; }
static int savedCount() { int n = 0; for (int k = 0; k < 3; k++) if (SSIDS[k]->length()) n++; return n; }

static int wfForgetArm = -1;          // first tap on "Esquecer" arms, second confirms
static int wfPage = 0;                // scan list page (5 per page)
static String wfTarget;               // network chosen in the list
static String kbText;                 // password being typed
static int kbMode = 0;                // 0 abc, 1 ABC, 2 123, 3 #+=
static bool kbShow = false;
static int wfSaveSlot = -2;           // where to save after a successful connection (-1 = don't save, -2 = nothing pending)

static void signalBars(int x, int y, int rssi, uint16_t bg) {
  int lvl = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
  for (int i = 0; i < 4; i++) {
    int h = 4 + i * 3;
    rect(x + i * 5, y + 13 - h, 3, h, i < lvl ? C_INK : C_LINE);
  }
  (void)bg;
}
static void lockIcon(int x, int y, uint16_t c, uint16_t bg) {
  rrect(x, y + 5, 9, 7, 1, c, c);
  spr.drawRoundRect(x + 2, y - yoff, 5, 8, 2, c);
  (void)bg;
}

static void startJoin(const String& ssid, const String& pass, int slot) {
  wfSaveSlot = slot;
  netConnectNow(ssid, pass);
  uiGoto(S_WIFI);
}
// the user chose a network: decide the slot, ask which one to replace when all 3 are taken
static void chooseAndJoin(const String& ssid, const String& pass) {
  int k = savedSlot(ssid);
  if (k < 0) k = freeSlot();
  if (k < 0) { uiGoto(S_WIFIREP); return; }
  startJoin(ssid, pass, k);
}

static void drawWifi() {
  header("WI-FI", true, "Config");
  // status
  card(8, 30, 304, 46);
  String l1, l2; uint16_t dot;
  if (wifiJoin == 1) { l1 = "Conectando em " + wifiJoinSsid + "…"; l2 = "aguarde até 20 s"; dot = C_OR; }
  else if (wifiJoin == 3) { l1 = "Não conectou em " + wifiJoinSsid; l2 = wifiJoinErr; dot = C_RED; }
  else if (wifiUp) { l1 = "Conectado em " + WiFi.SSID(); l2 = ipStr + " · sinal " + String(WiFi.RSSI()) + " dBm"; dot = C_GREEN; }
  else if (apMode) { l1 = "Sem rede · modo configuração"; l2 = "rede " AP_SSID " aberta pro celular"; dot = C_BLUE; }
  else { l1 = "Sem conexão"; l2 = savedCount() ? "procurando as redes salvas" : "escolha uma rede abaixo"; dot = C_RED; }
  circle(20, 44, 5, dot);
  text(32, 35, l1, fit(l1, 270, F_BS13, F_BM11), C_INK, C_CARD);
  text(32, 54, l2, fit(l2, 270, F_BM11, F_BM11), wifiJoin == 3 ? C_RED : C_MUTED, C_CARD);
  if (wifiJoin == 3) { rrect(222, 50, 82, 20, 4, C_PEACH, C_OR); text(263, 52, "Outra senha", F_BM11, C_INK, C_PEACH, 1); }
  // saved networks
  text(10, 82, "REDES SALVAS · " + String(savedCount()) + " DE 3", F_CS12, C_MUTED, C_BG);
  card(8, 98, 304, 96);
  for (int k = 0; k < 3; k++) {
    int y = 98 + k * 32;
    if (k) hline(9, y, 302, C_SOFT);
    const String& ss = *SSIDS[k];
    if (!ss.length()) { text(18, y + 8, "vazio", F_BM13, C_LINE, C_CARD); continue; }
    bool cur = wifiUp && WiFi.SSID() == ss;
    text(18, y + 8, ss, fit(ss, 180, F_BS13, F_BM11), C_INK, C_CARD);
    if (cur) text(18 + min(180, tw(ss, F_BS13)) + 8, y + 10, "em uso", F_BM11, C_GREEN, C_CARD);
    bool armed = wfForgetArm == k;
    rrect(222, y + 5, 82, 22, 4, armed ? C_RED : C_CARD, armed ? C_RED : C_LINE);
    text(263, y + 8, armed ? "Confirmar" : "Esquecer", F_BM11, armed ? C_WHITE : C_INK, armed ? C_RED : C_CARD, 1);
  }
  rrect(8, 200, 304, 34, 6, C_INK, C_INK);
  text(160, 208, "Procurar redes", F_BS13, C_WHITE, C_INK, 1);
}

static void drawWifiScan() {
  header("REDES POR PERTO", true, "Wi-Fi");
  if (wifiScanBusy && !wifiNetsN) { text(160, 100, "Procurando redes…", F_BS16, C_MUTED, C_BG, 1); return; }
  if (!wifiNetsN) { text(160, 90, "Nenhuma rede encontrada", F_BS16, C_INK, C_BG, 1); }
  int pages = max(1, (wifiNetsN + 4) / 5);
  wfPage = constrain(wfPage, 0, pages - 1);
  if (wifiNetsN) card(8, 30, 304, 162);
  for (int r = 0; r < 5; r++) {
    int i = wfPage * 5 + r;
    if (i >= wifiNetsN) break;
    int y = 30 + r * 32;
    if (r) hline(9, y, 302, C_SOFT);
    signalBars(16, y + 9, wifiNets[i].rssi, C_CARD);
    const String& ss = wifiNets[i].ssid;
    text(42, y + 8, ss, fit(ss, 180, F_BS13, F_BM11), C_INK, C_CARD);
    int k = savedSlot(ss);
    if (wifiUp && WiFi.SSID() == ss) text(296, y + 10, "conectada", F_BM11, C_GREEN, C_CARD, 2);
    else if (k >= 0) text(296, y + 10, "salva", F_BM11, C_ORT, C_CARD, 2);
    if (!wifiNets[i].open) lockIcon(234, y + 9, C_MUTED, C_CARD);
  }
  // bottom bar: pages + scan again
  rrect(8, 200, 52, 34, 6, C_CARD, C_LINE); text(34, 207, "‹", F_BS16, wfPage > 0 ? C_INK : C_LINE, C_CARD, 1);
  rrect(66, 200, 188, 34, 6, C_INK, C_INK);
  text(160, 208, wifiScanBusy ? "Procurando…" : "Procurar de novo", F_BS13, C_WHITE, C_INK, 1);
  rrect(260, 200, 52, 34, 6, C_CARD, C_LINE); text(286, 207, "›", F_BS16, wfPage < pages - 1 ? C_INK : C_LINE, C_CARD, 1);
}

// keyboard layout: 4 rows; row 3 has shift and backspace, row 4 has mode, space and connect
static const char* KB_ROWS[4][3] = {
  {"qwertyuiop", "asdfghjkl", "zxcvbnm"},
  {"QWERTYUIOP", "ASDFGHJKL", "ZXCVBNM"},
  {"1234567890", "@#$%&*-_+", ".,!?:;/"},
  {"1234567890", "()[]{}<>=", "~^|\\'\"`"},
};
static const int KB_Y[4] = {70, 112, 154, 196}, KB_H = 38, KB_W = 28, KB_G = 3;
static int kbRowX(int row, int n) { int w = n * KB_W + (n - 1) * KB_G; return row == 2 ? 6 + 42 + KB_G + (304 - 42 * 2 - KB_G * 2 - w) / 2 : (320 - w) / 2; }
static void key(int x, int y, int w, const String& lab, bool dark, const uint8_t* f = F_BS16) {
  rrect(x, y, w, KB_H, 5, dark ? C_INK : C_CARD, dark ? C_INK : C_LINE);
  text(x + w / 2, y + (f == F_BS16 ? 10 : 12), lab, f, dark ? C_WHITE : C_INK, dark ? C_INK : C_CARD, 1);
}
static void drawKeyb() {
  header("SENHA", true, "Redes");
  // field
  card(8, 30, 304, 34);
  text(16, 33, wfTarget, F_BM11, C_MUTED, C_CARD);
  String shown = kbShow ? kbText : String("");
  if (!kbShow) for (size_t i = 0; i < kbText.length(); i++) shown += "•";
  while (shown.length() && tw(shown, F_BS13) > 210) shown.remove(0, 1);
  int fx = 16 + tw(shown, F_BS13);
  text(16, 46, shown, F_BS13, C_INK, C_CARD);
  rect(fx + 1, 47, 2, 13, C_OR);
  rrect(244, 36, 62, 22, 4, C_SOFT, C_SOFT);
  text(275, 39, kbShow ? "ocultar" : "mostrar", F_BM11, C_INK, C_SOFT, 1);
  // keys
  const char** rows = KB_ROWS[kbMode];
  for (int r = 0; r < 3; r++) {
    int n = strlen(rows[r]), x0 = kbRowX(r, n);
    for (int i = 0; i < n; i++) key(x0 + i * (KB_W + KB_G), KB_Y[r], KB_W, String(rows[r][i]), false);
  }
  // shift / symbols page, backspace
  const char* sh = kbMode < 2 ? (kbMode == 1 ? "abc" : "ABC") : (kbMode == 3 ? "123" : "#+=");
  key(6, KB_Y[2], 42, sh, kbMode == 1 || kbMode == 3, F_BS13);
  key(320 - 6 - 42, KB_Y[2], 42, "apagar", false, F_BM11);
  // bottom row
  key(6, KB_Y[3], 52, kbMode >= 2 ? "abc" : "123", false, F_BS13);
  key(6 + 52 + KB_G, KB_Y[3], 154, "espaço", false, F_BM13);
  key(320 - 6 - 92, KB_Y[3], 92, "Conectar", true, F_BS13);
}
static void keybTap(int x, int y) {
  if (in(x, y, 240, 30, 72, 34)) { kbShow = !kbShow; redraw = true; return; }
  const char** rows = KB_ROWS[kbMode];
  for (int r = 0; r < 3; r++) {
    if (y < KB_Y[r] || y >= KB_Y[r] + KB_H + KB_G) continue;
    if (r == 2 && x < 6 + 42 + KB_G) { kbMode = kbMode == 0 ? 1 : kbMode == 1 ? 0 : kbMode == 2 ? 3 : 2; redraw = true; return; }
    if (r == 2 && x >= 320 - 6 - 42 - KB_G) { if (kbText.length()) kbText.remove(kbText.length() - 1); redraw = true; return; }
    int n = strlen(rows[r]), x0 = kbRowX(r, n);
    int i = (x - x0 + KB_G / 2) / (KB_W + KB_G);
    if (x < x0 - KB_G || i < 0 || i >= n) return;
    if (kbText.length() < 63) kbText += rows[r][i];
    if (kbMode == 1) kbMode = 0;                   // shift is one-shot
    redraw = true;
    return;
  }
  if (y >= KB_Y[3]) {
    if (x < 6 + 52 + KB_G) { kbMode = kbMode >= 2 ? 0 : 2; redraw = true; }
    else if (x < 320 - 6 - 92 - KB_G) { if (kbText.length() < 63) kbText += ' '; redraw = true; }
    else {
      if (kbText.length() < 8) { toast("Senha de Wi-Fi tem no mínimo 8 caracteres"); return; }
      chooseAndJoin(wfTarget, kbText);
    }
  }
}

static void drawWifiRep() {
  header("LISTA CHEIA", true, "Senha");
  wrap(10, 32, 300, "Já tem 3 redes salvas. Qual delas sai para entrar " + wfTarget + "?", F_BM13, C_INK, C_BG, 17, 2);
  for (int k = 0; k < 3; k++) {
    int y = 72 + k * 40;
    rrect(8, y, 304, 34, 6, C_CARD, C_LINE);
    text(18, y + 9, *SSIDS[k], fit(*SSIDS[k], 200, F_BS13, F_BM11), C_INK, C_CARD);
    text(302, y + 10, "substituir", F_BM11, C_RED, C_CARD, 2);
  }
  rrect(8, 198, 304, 34, 6, C_SOFT, C_LINE);
  text(160, 206, "Só conectar, sem salvar", F_BS13, C_INK, C_SOFT, 1);
}

// ---------- Config › Início: which items the home screen shows (4 slots, machines take 2) ----------
static int homeUsed() { int u = 0; for (int i = 0; i < homePickN(); i++) u += hiCost(homePick(i)); return u; }
static void drawHomeCfg() {
  header("TELA INICIAL", true, "Config");
  text(10, 30, "Toque na ordem em que quer ver. " + String(homeUsed()) + " de 4 espaços", F_BM11, C_MUTED, C_BG);
  for (int it = 1; it < HI_COUNT; it++) {
    int i = it - 1, x = 8 + (i % 2) * 154, y = 48 + (i / 2) * 40;
    int order = -1;
    for (int k = 0; k < homePickN(); k++) if (homePick(k) == it) order = k;
    bool on = order >= 0, avail = hiAvailable(it);
    rrect(x, y, 150, 34, 6, on ? C_INK : C_CARD, on ? C_INK : C_LINE);
    String nm = HI_NAME[it];
    if (it == HI_MAQ) nm += " (2)";
    text(x + 12, y + 9, nm, F_BS13, on ? C_WHITE : (avail ? C_INK : C_LINE), on ? C_INK : C_CARD);
    if (on) { circle(x + 134, y + 17, 10, C_OR); text(x + 134, y + 10, String(order + 1), F_BS13, C_INK, C_OR, 1); }
    else if (!avail) text(x + 140, y + 11, "módulo off", F_BM11, C_LINE, C_CARD, 2);
  }
  rrect(8, 212, 120, 24, 5, C_CARD, C_LINE);
  text(68, 216, "Padrão", F_BM11, C_INK, C_CARD, 1);
  text(312, 218, "o relógio vai pro topo se sair", F_BM11, C_MUTED, C_BG, 2);
}
static void homeCfgTap(int x, int y) {
  if (in(x, y, 8, 208, 124, 32)) { cfg.homeItems = 0x621; configSave(); redraw = true; return; }
  if (y < 48 || y >= 48 + 4 * 40) return;
  int it = 1 + ((y - 48) / 40) * 2 + (x >= 160 ? 1 : 0);
  if (it <= 0 || it >= HI_COUNT) return;
  int n = homePickN(), picks[4], m = 0;
  bool had = false;
  for (int k = 0; k < n; k++) { if (homePick(k) == it) had = true; else picks[m++] = homePick(k); }
  if (!had) {
    if (!hiAvailable(it)) { toast("Ligue o módulo em Config › Módulos"); return; }
    if (homeUsed() + hiCost(it) > 4) { toast("Não cabe: são 4 espaços"); return; }
    picks[m++] = it;
  }
  cfg.homeItems = 0;
  for (int k = 0; k < m; k++) cfg.homeItems |= (uint32_t)picks[k] << (k * 4);
  configSave();
  redraw = true;
}

// calibration
static int calStep = 0;
static int calRx[4], calRy[4];
static const int CAL_X[4] = {24, 296, 24, 296}, CAL_Y[4] = {24, 24, 216, 216};
static void drawCal() {
  rect(0, 0, 320, 240, C_BG);
  text(160, 92, "Calibrar o toque", F_BS16, C_INK, C_BG, 1);
  text(160, 116, "Toque no centro de cada alvo laranja", F_BM13, C_MUTED, C_BG, 1);
  text(160, 136, String(calStep + 1) + " de 4", F_BS13, C_ORT, C_BG, 1);
  int x = CAL_X[calStep], y = CAL_Y[calStep];
  circle(x, y, 10, C_OR);
  circle(x, y, 3, C_INK);
}
static void calTapRaw(int rx, int ry) {
  calRx[calStep] = rx; calRy[calStep] = ry;
  beep(2400, 30);
  if (++calStep < 4) { redraw = true; return; }
  // decide whether raw axes are swapped relative to the screen
  int dxRawX = abs(calRx[1] - calRx[0]), dxRawY = abs(calRy[1] - calRy[0]);
  bool swap = dxRawY > dxRawX;
  float a0 = swap ? (calRy[0] + calRy[2]) / 2.0f : (calRx[0] + calRx[2]) / 2.0f;   // left side
  float a1 = swap ? (calRy[1] + calRy[3]) / 2.0f : (calRx[1] + calRx[3]) / 2.0f;   // right side
  float b0 = swap ? (calRx[0] + calRx[1]) / 2.0f : (calRy[0] + calRy[1]) / 2.0f;   // top
  float b1 = swap ? (calRx[2] + calRx[3]) / 2.0f : (calRy[2] + calRy[3]) / 2.0f;   // bottom
  if (fabs(a1 - a0) < 200 || fabs(b1 - b0) < 200) { calStep = 0; toast("Não deu, tente de novo"); return; }
  cfg.calSwap = swap;
  cfg.calAx = (296 - 24) / (a1 - a0); cfg.calBx = 24 - cfg.calAx * a0;
  cfg.calAy = (216 - 24) / (b1 - b0); cfg.calBy = 24 - cfg.calAy * b0;
  cfg.calOk = true;
  configSave();
  logf("[ui] calibrated swap=%d ax=%.4f bx=%.1f ay=%.4f by=%.1f\n", swap, cfg.calAx, cfg.calBx, cfg.calAy, cfg.calBy);
  calStep = 0;
  uiGoto(savedCount() ? S_HOME : S_WIFI);
  toast("Toque calibrado");
}

// ---------- frame ----------
// ---------- avisos (notifications) ----------
static String clip(const String& in, int w, const uint8_t* f) {   // cut to width with an ellipsis, UTF-8 safe
  if (tw(in, f) <= w) return in;
  String s = in;
  while (s.length() && tw(s + "…", f) > w) {
    int n = s.length() - 1;
    while (n > 0 && ((uint8_t)s[n] & 0xC0) == 0x80) n--;
    s = s.substring(0, n);
  }
  return s + "…";
}
static uint16_t notifColor(uint8_t k) {
  return k == N_APROV ? C_WARN : k == N_REDE || k == N_MAQ || k == N_DECK ? C_RED : C_BLUE;
}
static const int AV_ROWS = 5;
static void drawAvisos() {
  header("AVISOS");
  if (!notifN) {
    card(8, 32, 304, 80);
    text(18, 42, "Nenhum aviso por enquanto", F_BS16, C_INK, C_CARD);
    wrap(18, 66, 284, "Aprovações do agente, máquinas que caem e quedas de internet aparecem aqui.", F_BM13, C_MUTED, C_CARD, 17, 3);
    return;
  }
  card(8, 30, 304, 36 * min((int)notifN, AV_ROWS) + 4);
  for (int i = 0; i < notifN && i < AV_ROWS; i++) {
    int y = 32 + i * 36;
    if (i) hline(16, y - 2, 288, C_LINE);
    circle(20, y + 10, 4, notifColor(notifs[i].kind));
    String when = notifs[i].at ? ago(notifs[i].at) : String("");
    int ww = when.length() ? tw(when, F_BM11) + 8 : 0;
    text(32, y + 3, clip(notifs[i].title, 270 - ww, F_BS13), F_BS13, C_INK, C_CARD);
    if (ww) text(302, y + 4, when, F_BM11, C_MUTED, C_CARD, 2);
    text(32, y + 19, clip(notifs[i].text, 270, F_BM11), F_BM11, C_MUTED, C_CARD);
  }
  rrect(214, 210, 98, 26, 6, C_CARD, C_LINE);
  text(263, 215, "Limpar", F_BS13, C_INK, C_CARD, 1);
}

// ---------- radio ----------
static const char* compass(int az) {
  static const char* P[8] = {"N", "NE", "L", "SE", "S", "SO", "O", "NO"};
  return P[((az % 360) + 22) / 45 % 8];
}
static void condChip(int x, int y, uint8_t c) {
  const char* lab[4] = {"—", "ruim", "regular", "boa"};
  uint16_t col = c == 3 ? C_GREEN : c == 2 ? C_WARN : c == 1 ? C_RED : C_SOFT;
  rrect(x, y, 70, 17, 4, col, col);
  text(x + 35, y + 2, lab[c & 3], F_BM11, c ? C_WHITE : C_MUTED, col, 1);
}
static void drawRadio() {
  header("RÁDIO · PROPAGAÇÃO");
  if (!live.rdOk && !live.issOk) {
    card(8, 32, 304, 90);
    text(18, 42, live.rdErr.length() ? live.rdErr : (wifiUp ? "Buscando propagação…" : "Sem Wi-Fi"), F_BS16, C_INK, C_CARD);
    wrap(18, 66, 284, "Índices solares e bandas de HF (N0NBH, hamqsl.com) e a próxima passagem da ISS, calculada aqui na placa.", F_BM13, C_MUTED, C_CARD, 17, 3);
    return;
  }
  const char* lab[4] = {"SFI", "K", "A", "MANCHAS"};
  int val[4] = {live.rdSfi, live.rdK, live.rdA, live.rdSpots};
  for (int i = 0; i < 4; i++) {
    int x = 8 + i * 77;
    card(x, 28, 72, 40);
    text(x + 8, 31, lab[i], F_CS12, C_MUTED, C_CARD);
    uint16_t col = (i == 1 && val[i] >= 5) || (i == 2 && val[i] >= 30) ? C_RED : C_INK;
    text(x + 8, 45, val[i] >= 0 ? String(val[i]) : String("—"), F_BS16, col, C_CARD);
  }
  card(8, 72, 304, 98);
  text(18, 76, "BANDA", F_CS12, C_MUTED, C_CARD);
  text(176, 76, "DIA", F_CS12, C_MUTED, C_CARD, 1);
  text(260, 76, "NOITE", F_CS12, C_MUTED, C_CARD, 1);
  const char* bn[4] = {"80–40 m", "30–20 m", "17–15 m", "12–10 m"};
  for (int i = 0; i < 4; i++) {
    int y = 94 + i * 19;
    text(18, y + 1, bn[i], F_BS13, C_INK, C_CARD);
    condChip(141, y, live.rdBand[0][i]);
    condChip(225, y, live.rdBand[1][i]);
  }
  card(8, 174, 304, 48);
  text(18, 178, "ISS · PRÓXIMA PASSAGEM", F_CS12, C_MUTED, C_CARD);
  time_t now; time(&now);
  if (live.issOk) {
    String when;
    if ((uint32_t)now >= live.issAos) when = "agora, até " + hhmm(live.issLos);
    else {
      long d = (long)live.issAos - (long)now;
      when = (d < 3600 ? "em " + String(d / 60) + " min · " : String("")) + hhmm(live.issAos);
      struct tm a, b; time_t ta = live.issAos; localtime_r(&ta, &a); localtime_r(&now, &b);
      if (a.tm_yday != b.tm_yday) when = String(DIAS[a.tm_wday]) + " " + hhmm(live.issAos);
    }
    text(18, 194, when, F_BS16, C_INK, C_CARD);
    String det = "máx " + String(live.issMaxEl) + "° · " + String((live.issLos - live.issAos) / 60) + " min · " + compass(live.issAzAos) + " → " + compass(live.issAzLos);
    text(302, 197, det, F_BM11, C_MUTED, C_CARD, 2);
  } else text(18, 194, live.rdErr.length() ? live.rdErr : "calculando…", F_BM13, C_MUTED, C_CARD);
  String foot = "N0NBH/hamqsl.com · CelesTrak";
  if (live.rdAt) foot += " · " + ago(live.rdAt);
  text(160, 226, foot, F_BM11, C_MUTED, C_BG, 1);
}

// ---------- blackout mode ----------
static void lastRow(int x, int y, const char* lab, const String& val, uint32_t at) {
  text(x, y, lab, F_BM11, C_MUTED, C_CARD);
  String a = at ? ago(at) : String("");
  text(x + 136, y, a, F_BM11, C_MUTED, C_CARD, 2);
  text(x, y + 13, val, F_BS13, C_INK, C_CARD);
}
static void drawApagao() {
  header("MODO APAGÃO");
  bool off = !netOnline && offlineSince;
  uint16_t bg = off ? C_RED : C_GREEN;
  rrect(8, 28, 304, 50, 6, bg, bg);
  text(18, 32, off ? "SEM INTERNET" : "INTERNET OK", F_BS16, C_WHITE, bg);
  if (off) text(302, 34, "há " + agoShort((millis() - offlineSince) / 1000), F_BS13, C_WHITE, bg, 2);
  text(18, 56, off ? (wifiUp ? "Wi-Fi conectado, mas sem saída para a internet" : "Wi-Fi fora do ar") : "Os dados voltam a atualizar sozinhos", F_BM11, C_WHITE, bg);
  // last known values
  card(8, 84, 150, 150);
  text(18, 88, "ÚLTIMOS DADOS", F_CS12, C_MUTED, C_CARD);
  lastRow(18, 104, "Bitcoin", live.btcOk ? "US$ " + fmtNum(live.btc, 0) : String("—"), live.btcAt);
  lastRow(18, 136, "Clima", live.wxOk ? fmtNum(live.wxTemp, 0) + "° " + wxShort(live.wxCode) : String("—"), live.wxAt);
  int on = 0; for (int i = 0; i < live.mqN; i++) if (live.mqOn[i]) on++;
  lastRow(18, 168, "Máquinas", live.mqN ? String(on) + " de " + live.mqN + " online" : String("—"), live.plAt);
  text(18, 206, timeOk() ? "hora: relógio interno" : "sem hora certa", F_BM11, C_MUTED, C_CARD);
  // what still works locally
  card(162, 84, 150, 150);
  text(172, 88, deckFresh() ? "DECK · OFF-GRID" : "AQUI E AGORA", F_CS12, C_MUTED, C_CARD);
  if (deckFresh()) {
    lastRow(172, 104, "Bateria", live.dkBat >= 0 ? String(live.dkBat) + "%" + (live.dkChg ? " · carregando" : "") : String("—"), 0);
    lastRow(172, 136, "Mesh", live.dkMeshN >= 0 ? String(live.dkMeshN) + (live.dkMeshN == 1 ? " nó" : " nós") : String("—"), 0);
    lastRow(172, 168, "GPS · 4G", String(live.dkFix ? "fix" : "sem fix") + " · " + (live.dkLte.length() ? live.dkLte : String("—")), 0);
    if (live.dkMsgN) text(172, 206, clip(live.dkMsgDe[0] + ": " + live.dkMsgTxt[0], 132, F_BM11), F_BM11, C_INK, C_CARD);
  } else {
    lastRow(172, 104, "Wi-Fi", wifiUp ? clip(WiFi.SSID(), 132, F_BS13) : String("desconectado"), 0);
    lastRow(172, 136, "Pi-hole (rede local)", live.phOk ? fmtNum(live.phPct, 1) + "% bloqueado" : String("—"), live.phAt);
    lastRow(172, 168, "Aprovações", hermes.promptId.length() ? String("1 pendente") : String("nenhuma"), 0);
    wrap(172, 200, 132, "Ligue o deck para mesh, GPS e 4G", F_BM11, C_MUTED, C_CARD, 13, 2);
  }
}

static void drawToast() {
  String m;
  if (millis() < toastUntil) m = toastMsg;
  else if (millis() < hermes.noticeUntil) m = hermes.notice;
  if (!m.length()) return;
  rrect(10, 196, 300, 38, 8, C_INK, C_INK);
  wrap(20, 199, 280, m, F_BM11, C_WHITE, C_INK, 15, 2);
}

static void drawScreen() {
  spr.fillScreen(C_BG);
  switch (curScreen) {
    case S_HOME: drawHome(); break;
    case S_BTC: drawBtc(); break;
    case S_CLIMA: drawClima(); break;
    case S_REDES: drawRedes(); break;
    case S_CASA: drawCasa(); break;
    case S_AGENTE: drawAgente(); break;
    case S_JOGOS: drawJogos(); break;
    case S_SNAKE: drawSnake(); break;
    case S_IR: drawIr(); break;
    case S_MESH: drawMesh(); break;
    case S_PIHOLE: drawPihole(); break;
    case S_AGENDA: drawAgenda(); break;
    case S_NEWS: drawNews(); break;
    case S_MAQ: drawMaq(); break;
    case S_APPS: drawApps(); break;
    case S_MODS: drawMods(); break;
    case S_CONFIG: drawConfig(); break;
    case S_QR: drawQr(); break;
    case S_CAL: drawCal(); break;
    case S_DECK: drawDeck(); break;
    case S_WIFI: drawWifi(); break;
    case S_WIFISCAN: drawWifiScan(); break;
    case S_KEYB: drawKeyb(); break;
    case S_WIFIREP: drawWifiRep(); break;
    case S_HOMECFG: drawHomeCfg(); break;
    case S_AVISOS: drawAvisos(); break;
    case S_RADIO: drawRadio(); break;
    case S_APAGAO: drawApagao(); break;
    case S_SSH: drawSsh(); break;
    case S_TANKS: drawTanks(); break;
  }
  if (curScreen != S_CAL && curScreen != S_SNAKE && curScreen != S_SSH) drawToast();
}

static void render() {
  bool scan = screenChanged && anim() && curScreen != S_SNAKE && curScreen != S_CAL && curScreen != S_TANKS && curScreen != S_SSH;
  screenChanged = false;
  curX = -1; curOn = false; pulseN = 0;
  xSemaphoreTake(dataLock, portMAX_DELAY);
  for (yoff = 0; yoff < 240; yoff += stripH) {
    drawScreen();
    if (!scan) { spr.pushSprite(0, yoff); continue; }
    // "scan" transition: the new screen is printed in bands with a green line running ahead
    for (int b = 0; b < stripH && yoff + b < 240; b += 12) {
      lcd.setClipRect(0, yoff + b, 320, 12);
      spr.pushSprite(0, yoff);
      lcd.clearClipRect();
      if (yoff + b + 12 < 240) lcd.drawFastHLine(0, yoff + b + 12, 320, C_GREEN);
      delay(5);
    }
  }
  xSemaphoreGive(dataLock);
  yoff = 0;
  lastBlink = millis();
}
void uiApplyTheme() {
  themePalette();
  curFont = nullptr;
  spr.unloadFont();
  redraw = true;
}
// boot log typed on the panel (Terminal themes with effects on)
static void bootLog() {
  if (!anim() || !cfg.calOk) return;
  lcd.fillScreen(C_BG);
  lcd.loadFont(T_BS13);
  lcd.setTextDatum(textdatum_t::top_left);
  int saved = 0; for (int k = 0; k < 3; k++) if ((k == 0 ? cfg.ssid : k == 1 ? cfg.ssid2 : cfg.ssid3).length()) saved++;
  int mods = 0; for (int i = 0; i < M_COUNT; i++) if ((cfg.mods >> i) & 1) mods++;
  String L[6] = {"cybercydeck " FW_VERSION, "memória .... " + String(ESP.getFreeHeap() / 1024) + " KB",
                 "tela ....... 320x240", "toque ...... calibrado", "redes ...... " + String(saved) + " de 3", "módulos .... " + String(mods) + " ativos"};
  int y = 30;
  for (int l = 0; l < 6; l++, y += 22) {
    lcd.setTextColor(C_GREEN, C_BG); lcd.drawString(">", 14, y);
    String shown;
    for (size_t i = 0; i < L[l].length(); i++) {
      shown += L[l][i];
      if ((uint8_t)L[l][i] >= 0xC0 || ((uint8_t)L[l][i] & 0xC0) == 0x80) { if (i + 1 < L[l].length() && ((uint8_t)L[l][i + 1] & 0xC0) == 0x80) continue; }
      lcd.setTextColor(l == 0 ? C_INK : C_MUTED, C_BG);
      lcd.drawString(shown, 30, y);
      delay(l == 0 ? 18 : 6);
    }
    if (l > 0) { lcd.setTextColor(C_GREEN, C_BG); lcd.drawString("ok", 280, y); }
    delay(60);
  }
  y += 6;
  lcd.setTextColor(C_INK, C_BG); lcd.drawString("> iniciando painel", 14, y);
  int cx = 14 + lcd.textWidth("> iniciando painel") + 3;
  for (int i = 0; i < 4; i++) { lcd.fillRect(cx, y, 8, 14, i % 2 ? C_BG : C_GREEN); delay(160); }
  lcd.unloadFont();
  screenChanged = true;
}

bool uiScreenshot(Stream& out) {
  xSemaphoreTake(dataLock, portMAX_DELAY);
  out.println("SHOT BEGIN 320 240");
  static unsigned char b64[900];
  for (yoff = 0; yoff < 240; yoff += stripH) {
    drawScreen();
    for (int r = 0; r < stripH && yoff + r < 240; r++) {
      const uint8_t* row = (const uint8_t*)spr.getBuffer() + r * 320 * 2;
      size_t olen = 0;
      mbedtls_base64_encode(b64, sizeof(b64), &olen, row, 640);
      out.printf("SHOT %d ", yoff + r);
      out.write(b64, olen);
      out.print("\n");
    }
    spr.pushSprite(0, yoff);
  }
  out.println("SHOT END");
  xSemaphoreGive(dataLock);
  yoff = 0;
  return true;
}

// ---------- navigation & input ----------
static const char* NAMES[S_COUNT] = {"home", "btc", "clima", "redes", "casa", "agente", "jogos", "snake", "ir", "mesh", "pihole", "agenda", "noticias", "maquinas", "apps", "mods", "config", "qr", "cal", "deck", "wifi", "wifiscan", "teclado", "wifirep", "inicio", "avisos", "radio", "apagao", "ssh", "tanques"};
const char* screenName(int s) { return (s >= 0 && s < S_COUNT) ? NAMES[s] : "?"; }
int screenFromName(const String& n) {
  for (int i = 0; i < S_COUNT; i++) if (n == NAMES[i]) return i;
  if (n == "bitcoin") return S_BTC;
  if (n == "weather") return S_CLIMA;
  if (n == "social") return S_REDES;
  return -1;
}
void uiGoto(int s) {
  if (s < 0 || s >= S_COUNT) return;
  int prev = curScreen;
  curScreen = s;
  if (prev != s) screenChanged = true;
  if (s == S_SNAKE) snakeReset();
  if (s == S_CAL) calStep = 0;
  if (s == S_CASA || s == S_PIHOLE) netRequestRefresh();
  if (s == S_AGENDA || s == S_NEWS || s == S_MAQ) netRequestPlaca();
  if (s == S_RADIO) netRequestRadio();
  if (s == S_SSH) termOpen();
  if (s == S_TANKS && prev != S_TANKS && (tkPhase == TK_OVER || !tkX[1])) tanksNew();
  if (s == S_AVISOS) notifUnread = 0;
  if (s == S_WIFI) { wfForgetArm = -1; if (wifiJoin >= 2) wifiJoin = wifiJoin == 3 ? 3 : 0; }
  if (s == S_WIFISCAN && prev != S_KEYB) { wfPage = 0; netRequestScan(); }
  redraw = true;
}
void uiRequestRedraw() { redraw = true; }

static bool in(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }

void uiTap(int x, int y) {
  logf("[ui] tap %d,%d on %s\n", x, y, screenName(curScreen));
  switch (curScreen) {
    case S_HOME:
      if (y < 26 && x > 230) { uiGoto(S_CONFIG); return; }
      if (y < 26) { uiGoto(hermes.promptId.length() ? S_AGENTE : (!netOnline && offlineSince) ? S_APAGAO : S_AVISOS); return; }
      homeLayout();
      for (int k = 0; k < 4; k++) {
        int it = homeSlot[k];
        if (it <= 0) continue;
        bool hit = it == HI_MAQ ? in(x, y, 6, SLOT_Y[k], 308, 76) : in(x, y, SLOT_X[k], SLOT_Y[k], 151, 76);
        if (!hit) continue;
        if (it == HI_CLOCK && x > SLOT_X[k] + 108 && y < SLOT_Y[k] + 52 && MOD_ON(M_CLIMA)) { uiGoto(S_CLIMA); return; }   // the small weather
        if (it == HI_CLOCK && !MOD_ON(M_AGENDA)) return;
        uiGoto(hiScreen(it));
        return;
      }
      if (y >= 190 && homeDockN) {
        int w = (306 - (homeDockN - 1) * 5) / homeDockN;
        int i = constrain((x - 7) / (w + 5), 0, homeDockN - 1);
        uiGoto(homeDock[i]);
      }
      return;
    case S_SNAKE: {
      if (y < 26 && x < 80) { uiGoto(S_JOGOS); return; }
      if (snDead) { snakeReset(); redraw = true; return; }
      int hx = snX[0] * SN_CELL + 8, hy = SN_Y + snY[0] * SN_CELL + 8;
      int dx = x - hx, dy = y - hy;
      int nd = abs(dx) > abs(dy) ? (dx > 0 ? 0 : 2) : (dy > 0 ? 1 : 3);
      if ((nd + 2) % 4 != snDir) snNext = nd;
      return;
    }
    case S_QR:
      if (y < 26 && x < 90) uiGoto(S_CONFIG);
      return;
    case S_APPS: {
      if (hitBack(x, y)) { uiGoto(S_HOME); return; }
      int a[15]; int n = appsList(a);
      for (int i = 0; i < n; i++) if (in(x, y, 8 + (i % 3) * 102, 29 + (i / 3) * APP_PITCH, 96, APP_H)) { uiGoto(a[i]); return; }
      return;
    }
    case S_WIFI:
      if (hitBack(x, y)) { uiGoto(S_CONFIG); return; }
      if (wifiJoin == 3 && in(x, y, 218, 46, 90, 28)) { wfTarget = wifiJoinSsid; kbText = ""; kbMode = 0; kbShow = false; uiGoto(S_KEYB); return; }
      for (int k = 0; k < 3; k++) {
        if (!SSIDS[k]->length() || !in(x, y, 216, 98 + k * 32, 96, 32)) continue;
        if (wfForgetArm != k) { wfForgetArm = k; redraw = true; return; }
        logf("[ui] forget wifi %s\n", SSIDS[k]->c_str());
        *SSIDS[k] = ""; *PASSES[k] = ""; configSave(); wfForgetArm = -1; toast("Rede esquecida"); redraw = true; return;
      }
      if (wfForgetArm >= 0) { wfForgetArm = -1; redraw = true; }
      if (in(x, y, 8, 196, 304, 40)) uiGoto(S_WIFISCAN);
      return;
    case S_WIFISCAN: {
      if (hitBack(x, y)) { uiGoto(S_WIFI); return; }
      if (y >= 196) {
        if (x < 62) wfPage = max(0, wfPage - 1);
        else if (x > 258) wfPage = min(max(0, (wifiNetsN - 1) / 5), wfPage + 1);
        else netRequestScan();
        redraw = true; return;
      }
      int r = (y - 30) / 32, i = wfPage * 5 + r;
      if (y < 30 || r > 4 || i >= wifiNetsN) return;
      wfTarget = wifiNets[i].ssid;
      int k = savedSlot(wfTarget);
      if (wifiNets[i].open) { chooseAndJoin(wfTarget, ""); return; }
      if (k >= 0) { startJoin(wfTarget, *PASSES[k], k); return; }   // already saved: use its password
      kbText = ""; kbMode = 0; kbShow = false;
      uiGoto(S_KEYB);
      return;
    }
    case S_HOMECFG:
      if (hitBack(x, y)) { uiGoto(S_CONFIG); return; }
      homeCfgTap(x, y);
      return;
    case S_SSH: sshTap(x, y); return;
    case S_TANKS: tanksTap(x, y); return;
    case S_AVISOS: {
      if (hitBack(x, y)) { uiGoto(S_HOME); return; }
      if (notifN && in(x, y, 214, 206, 98, 34)) { xSemaphoreTake(dataLock, portMAX_DELAY); notifN = 0; notifUnread = 0; xSemaphoreGive(dataLock); redraw = true; return; }
      int i = (y - 32) / 36;
      if (y < 32 || i >= AV_ROWS || i >= notifN) return;
      uint8_t k = notifs[i].kind;
      if (k == N_APROV || k == N_AGENTE) uiGoto(S_AGENTE);
      else if (k == N_REDE) uiGoto(S_APAGAO);
      else if (k == N_MAQ) uiGoto(S_MAQ);
      else if (k == N_DECK) uiGoto(S_DECK);
      return;
    }
    case S_KEYB:
      if (hitBack(x, y)) { uiGoto(S_WIFISCAN); return; }
      keybTap(x, y);
      return;
    case S_WIFIREP:
      if (hitBack(x, y)) { uiGoto(S_KEYB); return; }
      for (int k = 0; k < 3; k++) if (in(x, y, 8, 72 + k * 40, 304, 36)) { startJoin(wfTarget, kbText, k); return; }
      if (in(x, y, 8, 196, 304, 38)) startJoin(wfTarget, kbText, -1);
      return;
    case S_MODS:
      if (y < 26 && x < 90) { uiGoto(S_CONFIG); return; }
      for (int i = 0; i < M_COUNT; i++) {
        int bx = 8 + (i % 3) * 102, by = 30 + (i / 3) * 46;
        if (in(x, y, bx, by, 96, 42)) {
          cfg.mods ^= (1 << i);
          configSave();
          netRequestRefresh();
          redraw = true;
          logf("[ui] module %s %s\n", MOD_NAMES[i], (cfg.mods >> i) & 1 ? "on" : "off");
        }
      }
      return;
    default: break;
  }
  if (hitBack(x, y)) { uiGoto(S_HOME); return; }
  switch (curScreen) {
    case S_CASA:
      for (int i = 0; i < 4; i++) {
        int bx = 8 + (i % 2) * 156, by = 32 + (i / 2) * 70;
        if (in(x, y, bx, by, 148, 64)) {
          if (!cfg.haUrl.length() || !cfg.haEnt[i].length()) toast("Configure o Home Assistant pelo celular");
          else { netRequestHaToggle(i); toast("Enviando…", 1200); }
        }
      }
      break;
    case S_AGENTE:
      if (hermes.promptId.length() && y >= 192) {
        bool yes = x < 160;
        if (yes && millis() >= aprovArmUntil) { aprovArmUntil = millis() + 4000; redraw = true; break; }
        aprovArmUntil = 0;
        if (hermes.status != 3) { toast("Sem conexão com o Hermes: resposta não enviada"); break; }
        hermesSendPromptReply(yes);
        toast(yes ? "Aprovado" : "Recusado");
      }
      break;
    case S_JOGOS:
      if (in(x, y, 8, 32, 148, 72)) uiGoto(S_SNAKE);
      else if (in(x, y, 164, 32, 148, 72)) uiGoto(S_TANKS);
      else if (y > 30 && y < 190) toast("Esse jogo chega numa próxima versão");
      break;
    case S_IR:
      if (y >= 30 && y < 62) { irDev = constrain((x - 8) / 102, 0, 2); redraw = true; }
      else if (y >= 66 && y < 172) toast("Sem LED IR: ligue um no CN1 (GPIO27)");
      break;
    case S_CONFIG:
      if (in(x, y, 8, 30, 304, 28)) { uiGoto(S_WIFI); return; }
      if (in(x, y, 80, 58, 40, 32)) { cfg.bright = max(20, cfg.bright - 25); lcd.setBrightness(cfg.bright); configSave(); redraw = true; }
      else if (in(x, y, 246, 58, 40, 32)) { cfg.bright = min(255, cfg.bright + 25); lcd.setBrightness(cfg.bright); configSave(); redraw = true; }
      else if (in(x, y, 82, 94, 104, 28)) { cfg.theme = (cfg.theme + 1) % 3; configSave(); uiApplyTheme(); screenChanged = true; }
      else if (in(x, y, 188, 94, 64, 28)) { cfg.invert = !cfg.invert; lcd.invertDisplay(cfg.invert); configSave(); redraw = true; }
      else if (in(x, y, 254, 94, 50, 28)) { cfg.rgbOrder = !cfg.rgbOrder; configSave(); toast("Reiniciando pra aplicar…"); render(); delay(800); ESP.restart(); }
      else if (in(x, y, 14, 132, 72, 38)) uiGoto(S_CAL);
      else if (in(x, y, 86, 132, 72, 38)) uiGoto(S_MODS);
      else if (in(x, y, 158, 132, 72, 38)) uiGoto(S_HOMECFG);
      else if (in(x, y, 230, 132, 76, 38)) { toast("Reiniciando…"); render(); delay(500); ESP.restart(); }
      else if (in(x, y, 8, 184, 304, 36)) uiGoto(S_QR);
      break;
    default: break;
  }
}

// ---------- init & loop ----------
void uiInit() {
  spr.setColorDepth(16);
  const int tries[5] = {120, 80, 60, 48, 40};
  for (int h : tries) { if (spr.createSprite(320, h)) { stripH = h; break; } }
  logf("[ui] strip %dpx, free heap %u, largest %u\n", stripH, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  themePalette();
  if (!cfg.calOk) curScreen = S_CAL;
  bootLog();
  redraw = true;
}

static bool wasDown = false;
static uint32_t lastTouch = 0, lastMinuteDraw = 0, lastToastState = 0;

// ---------- night mode & alerts ----------
static uint32_t wakeUntil = 0, lastAlertCheck = 0;
static int appliedBright = -1, lastLed = -1;
static uint32_t beepedFor = 0;
static bool isNight() {
  if (!cfg.nightDim || !timeOk()) return false;
  time_t t; time(&t); struct tm tmv; localtime_r(&t, &tmv);
  return tmv.tm_hour >= 23 || tmv.tm_hour < 7;
}
static void applyBrightness() {
  int want = (isNight() && millis() > wakeUntil) ? min(cfg.bright, 14) : cfg.bright;
  if (want != appliedBright) { lcd.setBrightness(want); appliedBright = want; }
}
static void checkAlerts() {
  // LED: red = Hermes critical or a machine offline; orange = appointment within 15 min;
  // purple = BTC moved 5%+ in 24h; off otherwise. Beep only for the appointment, never at night.
  time_t now; time(&now);
  int led = 0;
  bool offline = false;
  for (int i = 0; i < live.mqN; i++) if (!live.mqOn[i]) offline = true;
  bool soon = false;
  if (MOD_ON(M_AGENDA) && live.agN && live.agQ[0] && !live.agDia[0]) {
    long s = (long)live.agQ[0] - (long)now;
    soon = s > -300 && s <= 15 * 60;
    if (soon && beepedFor != live.agQ[0]) {
      beepedFor = live.agQ[0];
      toast("Em " + String(max(0L, s / 60)) + " min: " + live.agT[0], 8000);
      if (cfg.alertBeep && !isNight()) { beep(1800, 90); delay(120); beep(2400, 90); }
    }
  }
  static bool lowWarned = false;
  bool lowBat = deckFresh() && live.dkBat >= 0 && live.dkBat <= 15 && !live.dkChg;
  if (lowBat && !lowWarned) { toast("Bateria do deck em " + String(live.dkBat) + "%", 6000); notifyAdd(N_DECK, "Bateria do deck baixa", String(live.dkBat) + "%, sem carregar"); }
  lowWarned = lowBat;
  // machines that went down or came back since the last feed
  static String wasOff = "|";
  static bool mqSeen = false;
  if (MOD_ON(M_MAQ) && live.plOk && live.mqN) {
    String nowOff = "|";
    for (int i = 0; i < live.mqN; i++) if (!live.mqOn[i]) nowOff += live.mqNome[i] + "|";
    if (mqSeen && nowOff != wasOff) {
      for (int i = 0; i < live.mqN; i++) {
        String key = "|" + live.mqNome[i] + "|";
        bool was = wasOff.indexOf(key) >= 0, isOff = nowOff.indexOf(key) >= 0;
        if (isOff && !was) notifyAdd(N_MAQ, live.mqNome[i] + " offline", "sem sinal há " + agoShort(live.mqAge[i]));
        else if (!isOff && was) notifyAdd(N_MAQ, live.mqNome[i] + " voltou", "online de novo");
      }
    }
    wasOff = nowOff; mqSeen = true;
  }
  if (hermes.promptId.length()) led = 4;   // a question from the agent comes first
  else if (lowBat || (MOD_ON(M_AGENTE) && live.saude == "critico") || (MOD_ON(M_MAQ) && offline) || (!netOnline && offlineSince)) led = 1;
  else if (soon) led = 2;
  else if (MOD_ON(M_BTC) && live.btcOk && fabs(live.btcChg) >= 5) led = 3;
  if (led != lastLed) {
    lastLed = led;
    if (led == 4) ledSet(0, 0, 1); else if (led == 1) ledSet(1, 0, 0); else if (led == 2) ledSet(1, 1, 0); else if (led == 3) ledSet(1, 0, 1); else ledSet(0, 0, 0);
  }
}

extern volatile uint32_t netHeartbeat;
void uiLoop() {
  if (!stripH) return;
  if (millis() - netHeartbeat > 90000UL) { Serial.println("[deck] rede travada há 90 s, reiniciando"); delay(100); ESP.restart(); }
  static bool hadPrompt = false;
  if (hermes.promptId.length() != hadPrompt) { hadPrompt = hermes.promptId.length(); lastAlertCheck = 0; aprovArmUntil = 0; redraw = true; }
  if (!lastAlertCheck || millis() - lastAlertCheck > 20000) { lastAlertCheck = millis(); checkAlerts(); }
  // blackout mode: 3 min without internet switches a passive screen to the offline one; back online, it returns
  static bool blackShown = false;
  bool black = cfg.apagao && !netOnline && offlineSince && millis() - offlineSince > 180000UL;
  if (black && !blackShown) {
    blackShown = true;
    const int passive[] = {S_HOME, S_BTC, S_CLIMA, S_REDES, S_AGENDA, S_NEWS, S_MAQ, S_PIHOLE, S_DECK, S_MESH, S_RADIO, S_APPS};
    for (int p : passive) if (curScreen == p) { uiGoto(S_APAGAO); break; }
  } else if (!black && blackShown) {
    blackShown = false;
    if (curScreen == S_APAGAO) { uiGoto(S_HOME); toast("Internet de volta"); }
  }
  // the approval countdown and the blackout timer tick every second
  static uint32_t lastTick = 0;
  if ((curScreen == S_AGENTE && hermes.promptUntil) || curScreen == S_APAGAO) {
    if (millis() - lastTick > 1000) { lastTick = millis(); redraw = true; }
  }
  if (aprovArmUntil && millis() > aprovArmUntil) { aprovArmUntil = 0; redraw = true; }
  applyBrightness();
  // touch (at night, the first tap only wakes the screen)
  if (curScreen != S_CAL && isNight() && millis() > wakeUntil) {
    int x, y;
    bool down = touchRead(x, y);
    if (down && !wasDown) { wakeUntil = millis() + 30000; lastTouch = millis(); }
    wasDown = down;
  } else if (curScreen == S_CAL) {
    int rx, ry;
    bool down = touchRaw(rx, ry);
    if (down && !wasDown && millis() - lastTouch > 300) { lastTouch = millis(); calTapRaw(rx, ry); }
    wasDown = down;
  } else {
    int x, y;
    bool down = touchRead(x, y);
    if (down && !wasDown && millis() - lastTouch > 180) { lastTouch = millis(); wakeUntil = millis() + 30000; uiTap(x, y); }
    wasDown = down;
  }
  // tanks animation and the board's turn
  if (curScreen == S_TANKS && (tkPhase == TK_FLY || tkPhase == TK_BOOM || (tkPhase == TK_AIM && tkTurn == 1)) && millis() - tkTick > 30) {
    tkTick = millis(); tanksStep(); redraw = true;
  }
  // terminal: redraw when the screen changed, at most ~20 times a second
  if (curScreen == S_SSH && vt.version != tmVersion && millis() - tmLastDraw > 50) { tmVersion = vt.version; tmLastDraw = millis(); redraw = true; }
  // snake tick
  if (curScreen == S_SNAKE && !snDead && millis() - snTick > 170) { snTick = millis(); snakeStep(); redraw = true; }
  // clock / toast expiry
  uint32_t minuteNow = (uint32_t)(time(nullptr) / 60);
  if (minuteNow != lastMinuteDraw) { lastMinuteDraw = minuteNow; redraw = true; }
  uint32_t ts = (millis() < toastUntil) || (millis() < hermes.noticeUntil);
  if (ts != lastToastState) { lastToastState = ts; redraw = true; }
  // Wi-Fi chosen on the screen: save it only after it really connected
  if (wifiJoin == 2 && wfSaveSlot > -2) {
    int k = wfSaveSlot; wfSaveSlot = -2;
    if (k >= 0) {
      bool keepPass = !kbText.length() && savedSlot(wifiJoinSsid) == k;   // reconnected with the saved password
      if (!keepPass) *PASSES[k] = kbText;
      *SSIDS[k] = wifiJoinSsid;
      configSave();
      toast("Conectado e salvo como rede " + String(k + 1));
    } else toast("Conectado (não salvo)");
    kbText = "";
    redraw = true;
  }
  if (wifiJoin == 3 && wfSaveSlot > -2) { wfSaveSlot = -2; redraw = true; }
  static bool lastFresh = false;
  bool fresh = deckFresh();
  if (fresh != lastFresh) { lastFresh = fresh; redraw = true; }
  if (redraw) { redraw = false; render(); }
  // terminal effects between frames: blinking cursor and pulsing machine dots
  if (anim() && curScreen != S_CAL && curScreen != S_SNAKE && !(isNight() && millis() > wakeUntil)) {
    if (curX > 0 && millis() - lastBlink > 530) {
      lastBlink = millis(); curOn = !curOn;
      lcd.fillRect(curX, curY + 1, 7, 13, curOn ? C_GREEN : curBg);
    }
    if (curScreen == S_HOME && pulseN && millis() - lastPulse > 650) {
      lastPulse = millis(); pulseOn = !pulseOn;
      for (int i = 0; i < pulseN; i++) lcd.fillCircle(pulseX[i], pulseY[i], 4, pulseOn ? C_GREEN : C_PEACH);
    }
  }
}
