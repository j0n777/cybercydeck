#include "ical.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// ---------- civil dates (days since 1970-01-01, proleptic Gregorian) ----------
static int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}
static void civilFromDays(int64_t z, int* y, int* m, int* d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = (int)(yoe + era * 400) + (*m <= 2);
}
static int weekday(int64_t days) { return (int)(((days % 7) + 7 + 4) % 7); }   // 0 = Sunday
static int daysInMonth(int y, int m) {
  static const int D[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0) ? 29 : D[m - 1];
}
static int64_t floorDiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

static uint32_t fnv(const char* s) {
  uint32_t h = 2166136261u;
  while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
  return h;
}
static int dayCode(const char* s) {   // "MO".."SU" -> 0=Sunday..6
  static const char* N[7] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
  for (int i = 0; i < 7; i++) if (s[0] == N[i][0] && s[1] == N[i][1]) return i;
  return -1;
}

void IcsAgenda::begin(int64_t now, int64_t horizon, long tzOffset) {
  now_ = now; horizon_ = horizon; tz_ = tzOffset;
  curLen_ = 0; prevLen_ = -1; inEvent_ = false; candN_ = 0; exclN_ = 0; events = 0;
}

void IcsAgenda::feed(const char* p, int len) {
  for (int i = 0; i < len; i++) {
    char c = p[i];
    if (c == '\r') continue;
    if (c != '\n') { if (curLen_ < (int)sizeof(cur_) - 1) cur_[curLen_++] = c; continue; }
    cur_[curLen_] = 0;
    if ((cur_[0] == ' ' || cur_[0] == '\t') && prevLen_ >= 0) {       // folded continuation
      int n = curLen_ - 1;
      if (prevLen_ + n > (int)sizeof(prev_) - 1) n = sizeof(prev_) - 1 - prevLen_;
      if (n > 0) { memcpy(prev_ + prevLen_, cur_ + 1, n); prevLen_ += n; prev_[prevLen_] = 0; }
    } else {
      if (prevLen_ >= 0) line(prev_);
      memcpy(prev_, cur_, curLen_ + 1); prevLen_ = curLen_;
    }
    curLen_ = 0;
  }
}

int64_t IcsAgenda::parseTime(const char* v, bool utcHint, bool* allDay) {
  int n = 0; while (isdigit((unsigned char)v[n])) n++;
  if (n < 8) return 0;
  int y = (v[0] - '0') * 1000 + (v[1] - '0') * 100 + (v[2] - '0') * 10 + (v[3] - '0');
  int mo = (v[4] - '0') * 10 + (v[5] - '0'), d = (v[6] - '0') * 10 + (v[7] - '0');
  int64_t days = daysFromCivil(y, mo, d);
  if (v[8] != 'T') { if (allDay) *allDay = true; return days * 86400 - tz_; }   // all-day: local midnight
  if (allDay) *allDay = false;
  const char* t = v + 9;
  int hh = (t[0] - '0') * 10 + (t[1] - '0'), mm = (t[2] - '0') * 10 + (t[3] - '0'), ss = (t[4] - '0') * 10 + (t[5] - '0');
  int64_t sec = days * 86400 + hh * 3600 + mm * 60 + ss;
  bool utc = t[6] == 'Z' || utcHint;
  return utc ? sec : sec - tz_;
}

void IcsAgenda::line(const char* s) {
  if (!strcmp(s, "BEGIN:VEVENT")) {
    inEvent_ = true; cancelled_ = false; allDay_ = false; dtStart_ = 0; recId_ = 0; uid_ = 0;
    summary_[0] = 0; rrule_[0] = 0; exN_ = 0;
    return;
  }
  if (!inEvent_) return;
  if (!strcmp(s, "END:VEVENT")) { inEvent_ = false; events++; endEvent(); return; }
  // NAME;PARAMS:VALUE  (a ':' inside a quoted parameter does not end the name)
  const char* colon = nullptr; bool q = false;
  for (const char* p = s; *p; p++) { if (*p == '"') q = !q; else if (*p == ':' && !q) { colon = p; break; } }
  if (!colon) return;
  const char* semi = strchr(s, ';');
  int nameLen = (int)((semi && semi < colon ? semi : colon) - s);
  char name[24]; if (nameLen > 23) return;
  memcpy(name, s, nameLen); name[nameLen] = 0;
  const char* val = colon + 1;
  if (!strcmp(name, "DTSTART")) dtStart_ = parseTime(val, false, &allDay_);
  else if (!strcmp(name, "RECURRENCE-ID")) recId_ = parseTime(val, false, nullptr);
  else if (!strcmp(name, "UID")) uid_ = fnv(val);
  else if (!strcmp(name, "STATUS")) cancelled_ = !strcmp(val, "CANCELLED");
  else if (!strcmp(name, "RRULE")) { strncpy(rrule_, val, sizeof(rrule_) - 1); rrule_[sizeof(rrule_) - 1] = 0; }
  else if (!strcmp(name, "EXDATE")) {
    const char* p = val;
    while (*p && exN_ < 12) {
      exdate_[exN_++] = parseTime(p, false, nullptr);
      const char* c = strchr(p, ','); if (!c) break; p = c + 1;
    }
  } else if (!strcmp(name, "SUMMARY")) {
    int o = 0;
    for (const char* p = val; *p && o < (int)sizeof(summary_) - 1; p++) {
      char c = *p;
      if (c == '\\' && p[1]) { p++; c = (*p == 'n' || *p == 'N') ? ' ' : *p; }
      summary_[o++] = c;
    }
    while (o > 0 && ((uint8_t)summary_[o - 1] & 0xC0) == 0x80) o--;    // never cut a UTF-8 character
    if (o > 0 && ((uint8_t)summary_[o - 1] & 0xC0) == 0xC0) o--;
    summary_[o] = 0;
  }
}

void IcsAgenda::addCandidate(int64_t start, bool allDay, bool fromRule) {
  int64_t end = start + (allDay ? 86400 : 1800);    // a timed event stays listed for 30 min after it starts
  if (end <= now_ || start >= horizon_) return;
  for (int i = 0; i < candN_; i++) if (cand_[i].uid == uid_ && cand_[i].start == start) return;
  if (candN_ == 24 && start >= cand_[23].start) return;
  int i = candN_ < 24 ? candN_++ : 23;
  while (i > 0 && cand_[i - 1].start > start) { cand_[i] = cand_[i - 1]; i--; }
  cand_[i].start = start; cand_[i].uid = uid_; cand_[i].allDay = allDay; cand_[i].fromRule = fromRule;
  strncpy(cand_[i].title, summary_[0] ? summary_ : "(sem título)", sizeof(cand_[i].title) - 1);
  cand_[i].title[sizeof(cand_[i].title) - 1] = 0;
}

void IcsAgenda::endEvent() {
  if (!dtStart_) return;
  if (recId_) {   // a moved or cancelled instance of a repeating event
    if (exclN_ < 48) excl_[exclN_++] = {uid_, recId_};
    if (!cancelled_) addCandidate(dtStart_, allDay_, false);
    return;
  }
  if (cancelled_) return;
  if (rrule_[0]) expandRule(); else addCandidate(dtStart_, allDay_, false);
}

void IcsAgenda::expandRule() {
  // RRULE:FREQ=WEEKLY;INTERVAL=2;BYDAY=MO,WE;UNTIL=20271231T000000Z;COUNT=10
  char freq[10] = ""; int interval = 1; long count = 0; int64_t until = 0; int monthDay = 0;
  int byDay[7]; int byOrd[7]; int byN = 0;
  char buf[160]; strncpy(buf, rrule_, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
  for (char* tok = strtok(buf, ";"); tok; tok = strtok(nullptr, ";")) {
    char* eq = strchr(tok, '='); if (!eq) continue;
    *eq = 0; const char* v = eq + 1;
    if (!strcmp(tok, "FREQ")) { strncpy(freq, v, sizeof(freq) - 1); freq[sizeof(freq) - 1] = 0; }
    else if (!strcmp(tok, "INTERVAL")) interval = atoi(v) > 0 ? atoi(v) : 1;
    else if (!strcmp(tok, "COUNT")) count = atol(v);
    else if (!strcmp(tok, "BYMONTHDAY")) monthDay = atoi(v);
    else if (!strcmp(tok, "UNTIL")) {
      bool dateOnly = false;
      until = parseTime(v, false, &dateOnly);
      if (dateOnly) until += 86399;   // the whole last day counts
    } else if (!strcmp(tok, "BYDAY")) {
      const char* p = v;
      while (*p && byN < 7) {
        int ord = 0;
        if (*p == '+' || *p == '-' || isdigit((unsigned char)*p)) { ord = atoi(p); while (*p == '+' || *p == '-' || isdigit((unsigned char)*p)) p++; }
        int d = dayCode(p);
        if (d >= 0) { byDay[byN] = d; byOrd[byN] = ord; byN++; }
        const char* c = strchr(p, ','); if (!c) break; p = c + 1;
      }
    }
  }
  int64_t local = dtStart_ + tz_;
  int64_t day0 = floorDiv(local, 86400), tod = local - day0 * 86400;
  int64_t winDay = floorDiv(now_ + tz_, 86400) - 1, endDay = floorDiv(horizon_ + tz_, 86400) + 1;
  long n = 0;          // occurrences produced so far (for COUNT)
  int guard = 0;
  auto emit = [&](int64_t d) -> bool {     // false = stop
    if (d < day0) return true;
    int64_t occ = d * 86400 + tod - tz_;
    if (until && occ > until) return false;
    if (count && n >= count) return false;
    n++;
    if (d > endDay) return false;
    for (int k = 0; k < exN_; k++) if (exdate_[k] == occ) return true;
    addCandidate(occ, allDay_, true);
    return true;
  };
  if (!strcmp(freq, "DAILY") || (!strcmp(freq, "WEEKLY") && !byN)) {
    int64_t period = (int64_t)interval * (strcmp(freq, "DAILY") ? 7 : 1);
    int64_t k = 0;
    if (!count && winDay > day0) k = (winDay - day0) / period;
    for (; guard < 4000; k++, guard++) if (!emit(day0 + k * period)) break;
  } else if (!strcmp(freq, "WEEKLY")) {
    int64_t weekStart = day0 - (weekday(day0) + 6) % 7;    // weeks start on Monday
    int order[7]; int on = 0;                                // BYDAY sorted Monday..Sunday
    for (int w = 1; w <= 7; w++) for (int i = 0; i < byN; i++) if (byDay[i] == w % 7) order[on++] = (w + 6) % 7;
    int64_t k = 0;
    if (!count && winDay > weekStart) { k = (winDay - weekStart) / (7LL * interval) - 1; if (k < 0) k = 0; }
    bool go = true;
    for (; go && guard < 2000; k++, guard++)
      for (int i = 0; i < on && go; i++) go = emit(weekStart + k * 7 * interval + order[i]);
  } else if (!strcmp(freq, "MONTHLY")) {
    int y, m, dd; civilFromDays(day0, &y, &m, &dd);
    int64_t k = 0;
    if (!count && winDay > day0) {
      int wy, wm, wd; civilFromDays(winDay, &wy, &wm, &wd);
      int64_t months = (int64_t)(wy - y) * 12 + (wm - m);
      k = months / interval - 1; if (k < 0) k = 0;
    }
    for (; guard < 600; k++, guard++) {
      int64_t mi = (int64_t)(m - 1) + k * interval;
      int yy = y + (int)floorDiv(mi, 12), mm = (int)(mi - floorDiv(mi, 12) * 12) + 1;
      int dim = daysInMonth(yy, mm);
      int64_t first = daysFromCivil(yy, mm, 1);
      if (byN && byOrd[0]) {   // e.g. 2TU = second Tuesday, -1FR = last Friday
        int want = byDay[0], ord = byOrd[0];
        int64_t d;
        if (ord > 0) d = first + ((want - weekday(first) + 7) % 7) + (int64_t)(ord - 1) * 7;
        else { int64_t last = first + dim - 1; d = last - ((weekday(last) - want + 7) % 7) + (int64_t)(ord + 1) * 7; }
        if (d < first || d >= first + dim) continue;
        if (!emit(d)) break;
      } else {
        int md = monthDay ? monthDay : dd;
        if (md < 0) md = dim + md + 1;
        if (md < 1 || md > dim) continue;
        if (!emit(first + md - 1)) break;
      }
    }
  } else if (!strcmp(freq, "YEARLY")) {
    int y, m, dd; civilFromDays(day0, &y, &m, &dd);
    int64_t k = 0;
    if (!count && winDay > day0) { int wy, wm, wd; civilFromDays(winDay, &wy, &wm, &wd); k = (wy - y) / interval - 1; if (k < 0) k = 0; }
    for (; guard < 300; k++, guard++) {
      int yy = y + (int)(k * interval);
      if (dd > daysInMonth(yy, m)) continue;
      if (!emit(daysFromCivil(yy, m, dd))) break;
    }
  } else addCandidate(dtStart_, allDay_, false);   // unknown rule: show the first one only
}

int IcsAgenda::finish(IcsEvent* out, int maxN) {
  if (prevLen_ >= 0) { line(prev_); prevLen_ = -1; }
  int o = 0;
  for (int i = 0; i < candN_ && o < maxN; i++) {
    bool skip = false;
    if (cand_[i].fromRule)
      for (int k = 0; k < exclN_; k++) if (excl_[k].uid == cand_[i].uid && excl_[k].start == cand_[i].start) { skip = true; break; }
    if (skip) continue;
    memcpy(out[o].title, cand_[i].title, sizeof(out[o].title));
    out[o].start = cand_[i].start; out[o].allDay = cand_[i].allDay;
    o++;
  }
  return o;
}
