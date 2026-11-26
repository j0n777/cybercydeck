// Small VT100/xterm screen for the SSH terminal: keeps a grid of characters with colours and
// understands what shells, ls, top, nano and less usually send (cursor moves, erase, scroll
// regions, SGR colours, the alternate screen, DEC line drawing). Plain C++: also builds on a PC.
#pragma once
#include <stdint.h>

#define VT_COLS 53
#define VT_ROWS 18

struct VtCell {
  uint16_t ch;     // Unicode code point (box drawing is mapped to ASCII)
  uint8_t fg, bg;  // 0..15 ANSI colours, 255 = default
  uint8_t attr;    // VT_BOLD | VT_INVERSE
};
enum { VT_BOLD = 1, VT_INVERSE = 2 };

class Vt {
 public:
  VtCell cell[VT_ROWS][VT_COLS];
  int cx = 0, cy = 0;
  bool cursorOn = true, appCursor = false;   // appCursor: arrows go as ESC O A instead of ESC [ A
  uint32_t version = 0;                      // goes up on every change (redraw when it moves)
  char reply[32]; int replyLen = 0;          // answers the server asked for (cursor position report)

  void reset();
  void feed(const char* data, int len);

 private:
  enum { GROUND, ESC, CSI, OSC, CHARSET } st_ = GROUND;
  int par_[16]; int np_ = 0; bool priv_ = false; bool oscEsc_ = false;
  uint8_t fg_ = 255, bg_ = 255, attr_ = 0;
  int top_ = 0, bot_ = VT_ROWS - 1;
  int sx_ = 0, sy_ = 0;
  bool wrapNext_ = false, lineDraw_ = false;
  uint32_t u8_ = 0; int u8n_ = 0;

  void put(uint32_t cp);
  void lineFeed();
  void scrollUp(int top, int bot, int n);
  void scrollDown(int top, int bot, int n);
  void clear(int r, int c0, int c1);
  void csi(char f);
  void sgr();
  int p(int i, int def) const { return i < np_ && par_[i] > 0 ? par_[i] : def; }
};
