// Keeps the last log lines in RAM so they can be read over Wi-Fi at /log (no USB needed).
// Only status lines go here: never URLs with query strings, tokens or passwords.
#include "app.h"
#include <stdarg.h>

static const int LOG_N = 24, LOG_W = 150;
static char ring[LOG_N][LOG_W];
static uint32_t ringAt[LOG_N];
static int head = 0, count = 0;
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

void logf(const char* fmt, ...) {
  char line[LOG_W];
  va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
  int n = strlen(line);
  if (n && line[n - 1] == '\n') line[--n] = 0;
  Serial.println(line);
  portENTER_CRITICAL(&mux);
  memcpy(ring[head], line, n + 1); ringAt[head] = millis() / 1000;
  head = (head + 1) % LOG_N; if (count < LOG_N) count++;
  portEXIT_CRITICAL(&mux);
}

String logDump() {
  String s;
  s.reserve(count * 60);
  for (int i = 0; i < count; i++) {
    int k = (head - count + i + LOG_N) % LOG_N;
    char t[16]; snprintf(t, sizeof(t), "%6lus ", (unsigned long)ringAt[k]);
    portENTER_CRITICAL(&mux); String l = ring[k]; portEXIT_CRITICAL(&mux);
    s += t; s += l; s += '\n';
  }
  return s;
}
