// Reads an RSS 2.0 or Atom stream byte by byte and keeps the first few headlines.
// Only titles are kept: the channel title and the <title> of each <item> / <entry>.
// Handles CDATA, tags inside titles, and the common entities (&amp; &#8217; &#x2019; ...).
// Plain C++: also builds on a PC for tests.
#pragma once
#include <stdint.h>

class RssReader {
 public:
  static const int MAXN = 5, TLEN = 140;
  char title[MAXN][TLEN];   // headlines, UTF-8
  char source[48];          // channel / feed title
  int n = 0;
  int maxN = MAXN;
  void begin(int keep);
  void feed(const char* chunk, int len);
  bool done() const { return n >= maxN; }   // enough headlines: the caller can stop the download

 private:
  enum { TEXT, TAG, CDATA } st = TEXT;
  char tag[24]; int tagLen = 0; bool tagOver = false;
  char cd[3];                    // last bytes, to spot "]]>" and "<![CDATA["
  int depthItem = 0;             // inside <item>/<entry>
  bool inTitle = false, sawItem = false;
  char buf[TLEN]; int bl = 0;
  char ent[10]; int el = -1;     // entity being read, -1 = none
  int skip = 0;
  bool latin1 = false; char head[100]; int hl = 0;   // ISO-8859-1 / windows-1252 feeds (seen in the XML prolog)                  // continuation bytes of a 4-byte UTF-8 char (emoji) still to drop
  void put(char c);
  void putUtf8(uint32_t cp);
  void endTag();
  void endTitle();
};
