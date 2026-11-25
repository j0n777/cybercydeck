// Reads an iCalendar (.ics) stream line by line and keeps the next few events.
// Made for the "secret address in iCal format" of Google Calendar (and any other .ics link):
// handles folded lines, all-day and timed events, UTC or local times, cancelled events,
// moved instances (RECURRENCE-ID), EXDATE and the common RRULEs (daily, weekly with BYDAY,
// monthly by day or by "2nd Tuesday", yearly, INTERVAL, COUNT, UNTIL).
// Times with TZID are read as the board's own time zone. Plain C++: also builds on a PC for tests.
#pragma once
#include <stdint.h>

struct IcsEvent {
  char title[64];
  int64_t start;      // epoch seconds (UTC)
  bool allDay;
};

class IcsAgenda {
 public:
  // now and horizon in epoch seconds; tzOffset = local time minus UTC, in seconds (-10800 for UTC-3)
  void begin(int64_t now, int64_t horizon, long tzOffset);
  void feed(const char* chunk, int len);   // raw bytes as they arrive (lines are unfolded here)
  int finish(IcsEvent* out, int maxN);     // events sorted by start; returns how many
  long events = 0;                         // VEVENTs seen (diagnostics)

 private:
  void line(const char* s);
  void endEvent();
  void addCandidate(int64_t start, bool allDay, bool fromRule);
  void expandRule();
  int64_t parseTime(const char* v, bool utcHint, bool* allDay);

  int64_t now_ = 0, horizon_ = 0; long tz_ = 0;
  char cur_[320]; int curLen_ = 0;        // line being unfolded
  char prev_[320]; int prevLen_ = -1;
  // current VEVENT
  bool inEvent_ = false, cancelled_ = false, allDay_ = false;
  int64_t dtStart_ = 0, recId_ = 0;
  uint32_t uid_ = 0;
  char summary_[64];
  char rrule_[160];
  int64_t exdate_[12]; int exN_ = 0;
  // results
  struct Cand { int64_t start; uint32_t uid; bool allDay, fromRule; char title[64]; };
  Cand cand_[24]; int candN_ = 0;
  struct Excl { uint32_t uid; int64_t start; };
  Excl excl_[48]; int exclN_ = 0;
};
