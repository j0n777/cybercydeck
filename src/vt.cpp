#include "vt.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void Vt::reset() {
  if (!cell) cell = (VtCell(*)[VT_COLS])calloc(VT_ROWS, sizeof(VtCell) * VT_COLS);
  fg_ = bg_ = 255; attr_ = 0;
  for (int r = 0; r < VT_ROWS; r++) clear(r, 0, VT_COLS - 1);
  cx = cy = 0; top_ = 0; bot_ = VT_ROWS - 1; sx_ = sy_ = 0;
  wrapNext_ = false; lineDraw_ = false; cursorOn = true; appCursor = false;
  st_ = GROUND; replyLen = 0; u8n_ = 0;
  version++;
}

void Vt::clear(int r, int c0, int c1) {
  for (int c = c0; c <= c1; c++) cell[r][c] = {' ', 255, bg_, 0};
}

void Vt::scrollUp(int t, int b, int n) {
  for (int k = 0; k < n; k++) {
    for (int r = t; r < b; r++) memcpy(cell[r], cell[r + 1], sizeof(VtCell) * VT_COLS);
    clear(b, 0, VT_COLS - 1);
  }
}
void Vt::scrollDown(int t, int b, int n) {
  for (int k = 0; k < n; k++) {
    for (int r = b; r > t; r--) memcpy(cell[r], cell[r - 1], sizeof(VtCell) * VT_COLS);
    clear(t, 0, VT_COLS - 1);
  }
}

void Vt::lineFeed() {
  if (cy == bot_) scrollUp(top_, bot_, 1);
  else if (cy < VT_ROWS - 1) cy++;
}

static uint16_t mapChar(uint32_t cp, bool lineDraw) {
  if (lineDraw && cp >= 0x60 && cp <= 0x7E) {   // DEC special graphics -> ASCII
    switch (cp) {
      case 'j': case 'k': case 'l': case 'm': case 'n': case 't': case 'u': case 'v': case 'w': return '+';
      case 'q': return '-';
      case 'x': return '|';
      case 'a': return '#';
      case '`': return '*';
      case '~': return '.';
      default: return (uint16_t)cp;
    }
  }
  if (cp >= 0x2500 && cp <= 0x257F) {           // box drawing
    static const uint16_t H[] = {0x2500, 0x2501, 0x2504, 0x2505, 0x2508, 0x2509, 0x254C, 0x254D, 0x2550};
    static const uint16_t V[] = {0x2502, 0x2503, 0x2506, 0x2507, 0x250A, 0x250B, 0x254E, 0x254F, 0x2551};
    for (uint16_t h : H) if (cp == h) return '-';
    for (uint16_t v : V) if (cp == v) return '|';
    return '+';
  }
  if (cp >= 0x2580 && cp <= 0x259F) return '#';  // blocks (progress bars)
  if (cp == 0x2026) return 0x2026;
  if (cp == 0x2014 || cp == 0x2013) return '-';
  if (cp == 0x2018 || cp == 0x2019) return '\'';
  if (cp == 0x201C || cp == 0x201D) return '"';
  if (cp == 0x2022 || cp == 0x00B7) return 0x00B7;
  if (cp < 0x20) return ' ';
  if (cp > 0xFF) return '?';
  return (uint16_t)cp;
}

void Vt::put(uint32_t cp) {
  if (wrapNext_) { cx = 0; lineFeed(); wrapNext_ = false; }
  cell[cy][cx] = {mapChar(cp, lineDraw_), fg_, bg_, attr_};
  if (cx == VT_COLS - 1) wrapNext_ = true; else cx++;
}

void Vt::sgr() {
  if (np_ == 0) { fg_ = bg_ = 255; attr_ = 0; return; }
  for (int i = 0; i < np_; i++) {
    int v = par_[i];
    if (v == 0) { fg_ = bg_ = 255; attr_ = 0; }
    else if (v == 1) attr_ |= VT_BOLD;
    else if (v == 7) attr_ |= VT_INVERSE;
    else if (v == 22) attr_ &= ~VT_BOLD;
    else if (v == 27) attr_ &= ~VT_INVERSE;
    else if (v >= 30 && v <= 37) fg_ = v - 30;
    else if (v >= 90 && v <= 97) fg_ = v - 90 + 8;
    else if (v == 39) fg_ = 255;
    else if (v >= 40 && v <= 47) bg_ = v - 40;
    else if (v >= 100 && v <= 107) bg_ = v - 100 + 8;
    else if (v == 49) bg_ = 255;
    else if ((v == 38 || v == 48) && i + 1 < np_) {
      uint8_t col = 255;
      if (par_[i + 1] == 5 && i + 2 < np_) {           // 256 colours -> nearest of 16
        int n = par_[i + 2];
        if (n < 16) col = n;
        else if (n >= 232) col = n < 244 ? 8 : 7;
        else { n -= 16; int r = n / 36, g = (n / 6) % 6, b = n % 6; col = (r > 2) | ((g > 2) << 1) | ((b > 2) << 2); if (r > 4 || g > 4 || b > 4) col += 8; }
        i += 2;
      } else if (par_[i + 1] == 2 && i + 4 < np_) {    // true colour -> nearest of 16
        int r = par_[i + 2], g = par_[i + 3], b = par_[i + 4];
        col = (r > 127) | ((g > 127) << 1) | ((b > 127) << 2);
        if (r > 200 || g > 200 || b > 200) col += 8;
        i += 4;
      }
      if (v == 38) fg_ = col; else bg_ = col;
    }
  }
}

void Vt::csi(char f) {
  int n = p(0, 1);
  switch (f) {
    case 'A': cy = clampi(cy - n, cy >= top_ ? top_ : 0, VT_ROWS - 1); break;
    case 'B': case 'e': cy = clampi(cy + n, 0, cy <= bot_ ? bot_ : VT_ROWS - 1); break;
    case 'C': case 'a': cx = clampi(cx + n, 0, VT_COLS - 1); break;
    case 'D': cx = clampi(cx - n, 0, VT_COLS - 1); break;
    case 'E': cx = 0; cy = clampi(cy + n, 0, VT_ROWS - 1); break;
    case 'F': cx = 0; cy = clampi(cy - n, 0, VT_ROWS - 1); break;
    case 'G': case '`': cx = clampi(n - 1, 0, VT_COLS - 1); break;
    case 'd': cy = clampi(n - 1, 0, VT_ROWS - 1); break;
    case 'H': case 'f': cy = clampi(p(0, 1) - 1, 0, VT_ROWS - 1); cx = clampi(p(1, 1) - 1, 0, VT_COLS - 1); break;
    case 'J': {
      int m = np_ ? par_[0] : 0;
      if (m == 0) { clear(cy, cx, VT_COLS - 1); for (int r = cy + 1; r < VT_ROWS; r++) clear(r, 0, VT_COLS - 1); }
      else if (m == 1) { clear(cy, 0, cx); for (int r = 0; r < cy; r++) clear(r, 0, VT_COLS - 1); }
      else for (int r = 0; r < VT_ROWS; r++) clear(r, 0, VT_COLS - 1);
      break;
    }
    case 'K': {
      int m = np_ ? par_[0] : 0;
      if (m == 0) clear(cy, cx, VT_COLS - 1); else if (m == 1) clear(cy, 0, cx); else clear(cy, 0, VT_COLS - 1);
      break;
    }
    case 'L': if (cy >= top_ && cy <= bot_) scrollDown(cy, bot_, n); break;
    case 'M': if (cy >= top_ && cy <= bot_) scrollUp(cy, bot_, n); break;
    case 'S': scrollUp(top_, bot_, n); break;
    case 'T': scrollDown(top_, bot_, n); break;
    case 'P': {   // delete characters
      n = clampi(n, 1, VT_COLS - cx);
      for (int c = cx; c < VT_COLS - n; c++) cell[cy][c] = cell[cy][c + n];
      clear(cy, VT_COLS - n, VT_COLS - 1);
      break;
    }
    case '@': {   // insert blanks
      n = clampi(n, 1, VT_COLS - cx);
      for (int c = VT_COLS - 1; c >= cx + n; c--) cell[cy][c] = cell[cy][c - n];
      clear(cy, cx, cx + n - 1);
      break;
    }
    case 'X': clear(cy, cx, clampi(cx + n - 1, 0, VT_COLS - 1)); break;
    case 'r':
      top_ = clampi(p(0, 1) - 1, 0, VT_ROWS - 1); bot_ = clampi(p(1, VT_ROWS) - 1, 0, VT_ROWS - 1);
      if (top_ >= bot_) { top_ = 0; bot_ = VT_ROWS - 1; }
      cx = 0; cy = 0;
      break;
    case 's': sx_ = cx; sy_ = cy; break;
    case 'u': cx = sx_; cy = sy_; break;
    case 'm': sgr(); break;
    case 'n':
      if (p(0, 0) == 6) replyLen = snprintf(reply, sizeof(reply), "\x1b[%d;%dR", cy + 1, cx + 1);
      else if (p(0, 0) == 5) replyLen = snprintf(reply, sizeof(reply), "\x1b[0n");
      break;
    case 'c': if (!priv_) replyLen = snprintf(reply, sizeof(reply), "\x1b[?1;2c"); break;   // "I am a VT100"
    case 'h': case 'l': {
      bool on = f == 'h';
      for (int i = 0; i < np_; i++) {
        if (!priv_) continue;
        if (par_[i] == 25) cursorOn = on;
        else if (par_[i] == 1) appCursor = on;
        else if (par_[i] == 1049 || par_[i] == 47 || par_[i] == 1047) {   // alternate screen: start clean
          for (int r = 0; r < VT_ROWS; r++) clear(r, 0, VT_COLS - 1);
          if (on) { sx_ = cx; sy_ = cy; } else { cx = sx_; cy = sy_; }
        }
      }
      break;
    }
    default: break;
  }
  wrapNext_ = false;
}

void Vt::feed(const char* data, int len) {
  if (!cell) reset();
  for (int i = 0; i < len; i++) {
    uint8_t b = (uint8_t)data[i];
    switch (st_) {
      case GROUND:
        if (u8n_) {                                    // continuing a UTF-8 sequence
          if ((b & 0xC0) == 0x80) { u8_ = (u8_ << 6) | (b & 0x3F); if (--u8n_ == 0) put(u8_); continue; }
          u8n_ = 0;
        }
        if (b == 0x1B) { st_ = ESC; continue; }
        if (b == '\r') { cx = 0; wrapNext_ = false; }
        else if (b == '\n' || b == 0x0B || b == 0x0C) { lineFeed(); wrapNext_ = false; }
        else if (b == '\b') { if (cx > 0) cx--; wrapNext_ = false; }
        else if (b == '\t') { cx = clampi((cx / 8 + 1) * 8, 0, VT_COLS - 1); }
        else if (b == 0x0E) lineDraw_ = true;          // SO: G1 (line drawing)
        else if (b == 0x0F) lineDraw_ = false;         // SI: back to G0
        else if (b < 0x20 || b == 0x7F) { }            // other controls (BEL...)
        else if (b < 0x80) put(b);
        else if ((b & 0xE0) == 0xC0) { u8_ = b & 0x1F; u8n_ = 1; }
        else if ((b & 0xF0) == 0xE0) { u8_ = b & 0x0F; u8n_ = 2; }
        else if ((b & 0xF8) == 0xF0) { u8_ = b & 0x07; u8n_ = 3; }
        else put(b);                                   // stray byte: show as Latin-1
        break;
      case ESC:
        st_ = GROUND;
        if (b == '[') { st_ = CSI; np_ = 0; par_[0] = 0; priv_ = false; }
        else if (b == ']') { st_ = OSC; oscEsc_ = false; }
        else if (b == '(' || b == ')') { st_ = CHARSET; par_[0] = b; }
        else if (b == '7') { sx_ = cx; sy_ = cy; }
        else if (b == '8') { cx = sx_; cy = sy_; }
        else if (b == 'D') lineFeed();
        else if (b == 'E') { cx = 0; lineFeed(); }
        else if (b == 'M') { if (cy == top_) scrollDown(top_, bot_, 1); else if (cy > 0) cy--; }
        else if (b == 'c') reset();
        else if (b == '=' || b == '>') { }             // keypad modes
        break;
      case CHARSET:
        if (par_[0] == '(') lineDraw_ = b == '0';
        st_ = GROUND;
        break;
      case CSI:
        if (b >= '0' && b <= '9') { if (np_ == 0) np_ = 1; par_[np_ - 1] = par_[np_ - 1] * 10 + (b - '0'); }
        else if (b == ';' || b == ':') { if (np_ == 0) np_ = 1; if (np_ < 16) { par_[np_] = 0; np_++; } }
        else if (b == '?' || b == '>' || b == '=') priv_ = true;
        else if (b >= 0x40 && b <= 0x7E) { csi((char)b); st_ = GROUND; }
        else if (b == 0x1B) st_ = ESC;                 // broken sequence
        break;
      case OSC:                                        // window title etc.: ignored until BEL or ESC \ .
        if (b == 0x07) st_ = GROUND;
        else if (oscEsc_) st_ = GROUND;
        else oscEsc_ = b == 0x1B;
        break;
    }
  }
  version++;
}
