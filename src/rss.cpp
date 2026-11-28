#include "rss.h"
#include <string.h>
#include <stdlib.h>

void RssReader::begin(int keep) {
  n = 0; maxN = keep < 1 ? 1 : keep > MAXN ? MAXN : keep;
  source[0] = 0; st = TEXT; tagLen = 0; tagOver = false; depthItem = 0;
  inTitle = false; sawItem = false; bl = 0; el = -1; skip = 0; latin1 = false; hl = 0; memset(cd, 0, sizeof(cd));
}

void RssReader::putUtf8(uint32_t cp) {
  if (cp < 0x80) { put((char)cp); return; }
  if (cp == 0xA0) { put(' '); return; }
  char o[4]; int k;
  if (cp < 0x800) { o[0] = 0xC0 | (cp >> 6); o[1] = 0x80 | (cp & 0x3F); k = 2; }
  else if (cp < 0x10000) { o[0] = 0xE0 | (cp >> 12); o[1] = 0x80 | ((cp >> 6) & 0x3F); o[2] = 0x80 | (cp & 0x3F); k = 3; }
  else return;                                 // emoji and friends: the screen fonts don't have them
  if (bl + k < TLEN) { memcpy(buf + bl, o, k); bl += k; }
}

void RssReader::put(char c) {
  if (!inTitle) return;
  if (skip) { skip--; return; }
  if ((unsigned char)c >= 0xF0) { skip = 3; return; }        // emoji: the screen fonts don't have them
  if (c == '\r' || c == '\n' || c == '\t') c = ' ';
  if (c == ' ' && (bl == 0 || buf[bl - 1] == ' ')) return;   // collapse spaces
  if (bl < TLEN - 1) buf[bl++] = c;
}

void RssReader::endTitle() {
  while (bl && buf[bl - 1] == ' ') bl--;
  // don't cut a UTF-8 character in half
  while (bl && (buf[bl - 1] & 0xC0) == 0x80) { int s = bl - 1; while (s && (buf[s] & 0xC0) == 0x80) s--;
    int need = (buf[s] & 0xE0) == 0xC0 ? 2 : (buf[s] & 0xF0) == 0xE0 ? 3 : 4; if (bl - s >= need) break; bl = s; }
  buf[bl] = 0;
  if (depthItem > 0) { if (bl && n < maxN) { strcpy(title[n], buf); n++; } }
  else if (!sawItem && !source[0]) { strncpy(source, buf, sizeof(source) - 1); source[sizeof(source) - 1] = 0; }
  inTitle = false; bl = 0;
}

void RssReader::endTag() {
  tag[tagLen] = 0;
  const char* t = tag;
  bool close = t[0] == '/'; if (close) t++;
  const char* colon = strchr(t, ':'); if (colon) t = colon + 1;   // atom:title, media:title...
  int l = 0; while (t[l] && t[l] != ' ' && t[l] != '/' && t[l] != '\t' && t[l] != '\n' && t[l] != '\r') l++;
  bool selfClose = tagLen && tag[tagLen - 1] == '/';
  auto is = [&](const char* w) { return (int)strlen(w) == l && !strncmp(t, w, l); };
  bool tagIsMedia = colon && !strncmp(tag + (close ? 1 : 0), "media", 5);
  if (is("item") || is("entry")) {
    if (close) { if (depthItem) depthItem--; }
    else if (!selfClose) { depthItem++; sawItem = true; }
  } else if (is("title") && !tagIsMedia) {
    if (close) { if (inTitle) endTitle(); }
    else if (!selfClose && (depthItem > 0 || !sawItem)) { inTitle = true; bl = 0; }
  }
}

void RssReader::feed(const char* s, int len) {
  for (int i = 0; i < len && !done(); i++) {
    char c = s[i];
    if (hl < (int)sizeof(head) - 1) {            // the encoding is named in the first bytes
      head[hl++] = c >= 'A' && c <= 'Z' ? c + 32 : c; head[hl] = 0;
      if (c == '>' && strstr(head, "<?xml")) { latin1 = strstr(head, "8859-1") || strstr(head, "1252") || strstr(head, "latin1"); hl = sizeof(head); }
    }
    if (latin1 && (unsigned char)c >= 0x80 && st != TAG) { putUtf8((unsigned char)c); continue; }
    if (st == CDATA) {
      cd[0] = cd[1]; cd[1] = cd[2]; cd[2] = c;
      if (cd[0] == ']' && cd[1] == ']' && c == '>') {
        if (bl >= 2) bl -= 2;                // drop the "]]" we already wrote
        st = TEXT; memset(cd, 0, sizeof(cd));
      } else put(c);
      continue;
    }
    if (st == TAG) {
      if (c == '>') { st = TEXT; if (!tagOver) endTag(); continue; }
      if (tagLen < (int)sizeof(tag) - 1) tag[tagLen++] = c; else tagOver = true;
      if (tagLen == 8 && !strncmp(tag, "![CDATA[", 8)) { st = CDATA; memset(cd, 0, sizeof(cd)); }
      continue;
    }
    // TEXT
    if (el >= 0) {
      if (c == ';' || el >= 9) {
        ent[el] = 0; el = -1;
        uint32_t cp = 0;
        if (ent[0] == '#') cp = (ent[1] == 'x' || ent[1] == 'X') ? strtoul(ent + 2, 0, 16) : strtoul(ent + 1, 0, 10);
        else if (!strcmp(ent, "amp")) cp = '&'; else if (!strcmp(ent, "lt")) cp = '<'; else if (!strcmp(ent, "gt")) cp = '>';
        else if (!strcmp(ent, "quot")) cp = '"'; else if (!strcmp(ent, "apos")) cp = '\''; else if (!strcmp(ent, "nbsp")) cp = ' ';
        if (cp == 0x2018 || cp == 0x2019) cp = '\''; else if (cp == 0x201C || cp == 0x201D) cp = '"';
        else if (cp == 0x2013 || cp == 0x2014) cp = '-'; else if (cp == 0x2026) { put('.'); put('.'); cp = '.'; }
        if (cp) putUtf8(cp);
        continue;
      }
      ent[el++] = c; continue;
    }
    if (c == '&') { el = 0; continue; }
    if (c == '<') {
      // a "<" inside a title that was escaped twice (&lt;b&gt;) is already text; a real tag starts here
      st = TAG; tagLen = 0; tagOver = false; continue;
    }
    put(c);
  }
}
