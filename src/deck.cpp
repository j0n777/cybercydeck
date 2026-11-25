// Link with the cyberdeck host (an Orange Pi / Raspberry Pi running deckd).
// The host writes one line per update on the CYD's USB serial:  deck {json}
// Format: docs/deck-protocol.md. The CYD never writes anything back except its usual log.
#include "app.h"
#include <ArduinoJson.h>
#include <sys/time.h>

bool deckFresh() { return live.dkAt && millis() - live.dkAt < 15000; }

void deckLine(const String& json) {
  JsonDocument d;
  if (deserializeJson(d, json)) { Serial.println("ERR deck json"); return; }
  // offgrid there is no NTP: take the clock from the host when ours is not set
  uint32_t t = d["t"] | 0;
  time_t now; time(&now);
  if (t > 1700000000 && (now < 1700000000 || labs((long)now - (long)t) > 120)) {
    struct timeval tv = {(time_t)t, 0};
    settimeofday(&tv, nullptr);
  }
  xSemaphoreTake(dataLock, portMAX_DELAY);
  live.dkHost = String((const char*)(d["host"] | "deck"));
  live.dkBat = d["bat"]["pct"] | -1;
  live.dkV = d["bat"]["v"] | 0.0f;
  live.dkChg = d["bat"]["chg"] | false;
  live.dkCpu = d["cpu"] | -1; live.dkRam = d["ram"] | -1; live.dkTemp = d["temp"] | -1;
  live.dkUp = d["up"] | 0;
  live.dkWifi = String((const char*)(d["wifi"]["ssid"] | ""));
  live.dkIp = String((const char*)(d["wifi"]["ip"] | ""));
  live.dkRssi = d["wifi"]["rssi"] | 0;
  live.dkLte = String((const char*)(d["lte"]["op"] | ""));
  live.dkSig = d["lte"]["sig"] | -1;
  live.dkFix = d["gps"]["fix"] | false;
  live.dkSats = d["gps"]["sats"] | -1;
  live.dkMeshN = d["mesh"]["nodes"] | -1;
  live.dkMsgN = 0;
  for (JsonObject m : d["mesh"]["msgs"].as<JsonArray>()) {
    if (live.dkMsgN >= 3) break;
    live.dkMsgDe[live.dkMsgN] = String((const char*)(m["de"] | "?"));
    live.dkMsgTxt[live.dkMsgN] = String((const char*)(m["txt"] | ""));
    live.dkMsgT[live.dkMsgN] = m["t"] | 0;
    live.dkMsgN++;
  }
  live.dkAlert = String((const char*)(d["alert"] | ""));
  live.dkAt = millis();
  if (!live.dkAt) live.dkAt = 1;
  xSemaphoreGive(dataLock);
  if (curScreen == S_DECK || curScreen == S_MESH) uiRequestRedraw();
}
