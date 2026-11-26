// CyberCYDeck — dashboard and cyberdeck companion firmware for the ESP32-2432S028 "Cheap Yellow Display".
#include "app.h"
#include <esp_task_wdt.h>

LGFX lcd;

static String serialLine;

static void handleCommand(String cmd) {
  cmd.trim();
  if (!cmd.length()) return;
  int sp = cmd.indexOf(' ');
  String head = sp > 0 ? cmd.substring(0, sp) : cmd;
  String arg = sp > 0 ? cmd.substring(sp + 1) : "";
  if (head == "shot") uiScreenshot(Serial);
  else if (head == "nav") {
    int s = screenFromName(arg);
    if (s >= 0) { uiGoto(s); Serial.printf("OK nav %s\n", screenName(s)); } else Serial.println("ERR screen");
  } else if (head == "tap") {
    int x = arg.toInt(), y = arg.substring(arg.indexOf(' ') + 1).toInt();
    uiTap(x, y);
    Serial.println("OK tap");
  } else if (head == "status") {
    Serial.printf("STATUS fw=%s screen=%s wifi=%d ap=%d ip=%s heap=%u maxblk=%u btc=%.0f wx=%d soc=%d hermes=%d cal=%d up=%lus\n", FW_VERSION,
                  screenName(curScreen), (int)wifiUp, (int)apMode, ipStr.c_str(), ESP.getFreeHeap(), ESP.getMaxAllocHeap(), live.btc, (int)live.wxOk,
                  (int)live.socOk, hermes.status, (int)cfg.calOk, millis() / 1000);
  } else if (head == "touch") {
    int rx, ry;
    bool d = touchRaw(rx, ry);
    Serial.printf("TOUCH down=%d raw=%d,%d\n", d, rx, ry);
  } else if (head == "tpdump") { touchDump(Serial);
  } else if (head == "invert") { cfg.invert = arg.toInt(); lcd.invertDisplay(cfg.invert); configSave(); uiRequestRedraw(); Serial.println("OK"); }
  else if (head == "rgb") { cfg.rgbOrder = arg.toInt(); configSave(); Serial.println("OK restart to apply"); }
  else if (head == "skipcal") { cfg.calOk = false; uiGoto(S_HOME); Serial.println("OK"); }
  else if (head == "led") { ledColorName(arg); Serial.println("OK"); }
  else if (head == "beep") { beep(); Serial.println("OK"); }
  else if (head == "set") {
    // set <key> <value> — for fixing config over USB
    int sp2 = arg.indexOf(' ');
    String k = sp2 > 0 ? arg.substring(0, sp2) : arg, v = sp2 > 0 ? arg.substring(sp2 + 1) : "";
    v.trim();
    bool ok = true;
    if (k == "n8n") cfg.n8nUrl = v;
    else if (k == "phurl") cfg.phUrl = v;
    else if (k == "phpass") cfg.phPass = v;
    else if (k == "city") { cfg.city = v; cfg.lat = NAN; cfg.lon = NAN; }
    else if (k == "mods") cfg.mods = (uint16_t)strtol(v.c_str(), nullptr, 16);
    else if (k == "token") cfg.feedToken = v;
    else if (k == "ical") cfg.icalUrl = v;
    else if (k == "feed") cfg.feedUrl = v;
    else if (k == "nome") cfg.nome = v;
    else if (k == "night") cfg.nightDim = v.toInt();
    else if (k == "theme") { cfg.theme = constrain(v.toInt(), 0, 2); uiApplyTheme(); }
    else if (k == "anim") cfg.anim = v.toInt();
    else if (k == "apagao") cfg.apagao = v.toInt();
    else if (k == "beep") cfg.alertBeep = v.toInt();
    else ok = false;
    if (ok) { configSave(); netRequestRefresh(); uiRequestRedraw(); Serial.printf("OK set %s\n", k.c_str()); }
    else Serial.println("ERR key (nome, n8n, feed, token, phurl, phpass, city, mods, night, beep, theme 0|1|2, anim 0|1, apagao 0|1, ical)");
  }
  else if (head == "deck") deckLine(arg);   // status from the cyberdeck host, sent every few seconds
  else if (head == "hermes") { hermesInject(arg); Serial.println("OK"); }   // test: feed a Hermes frame
  else if (head == "notify") { notifyAdd(N_AGENTE, "Teste", arg); Serial.println("OK"); }
  else if (head == "cfg") {
    Serial.printf("CFG nome=%s feed=%s n8n=%s phurl=%s phpass=%s token=%s ical=%s night=%d beep=%d mods=%03X city=%s ssid=%s ssid2=%s ssid3=%s\n", cfg.nome.c_str(), cfg.feedUrl.c_str(), cfg.n8nUrl.c_str(), cfg.phUrl.c_str(),
                  cfg.phPass.length() ? "(set)" : "", cfg.feedToken.length() ? "(set)" : "", cfg.icalUrl.length() ? "(set)" : "", cfg.nightDim, cfg.alertBeep, cfg.mods, cfg.city.c_str(), cfg.ssid.c_str(), cfg.ssid2.c_str(), cfg.ssid3.c_str());
  }
  else if (head == "restart") ESP.restart();
  else if (head == "factory") { configFactoryReset(); Serial.println("OK factory reset, restarting"); delay(300); ESP.restart(); }
  else Serial.println("ERR unknown (shot, nav <s>, tap x y, status, set <k> <v>, cfg, touch, invert 0|1, rgb 0|1, skipcal, led <c>, beep, restart, factory)");
}

void setup() {
  Serial.begin(115200);
  delay(50);
  Serial.printf("\n[deck] " PROJECT_NAME " %s booting, heap %u largest %u\n", FW_VERSION, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  pinMode(PIN_LED_R, OUTPUT); pinMode(PIN_LED_G, OUTPUT); pinMode(PIN_LED_B, OUTPUT);
  ledSet(0, 0, 0);
  pinMode(PIN_LDR, INPUT);
  configLoad();
  lcd.setup(cfg.invert, cfg.rgbOrder);
  lcd.init();
  lcd.setRotation(1);
  lcd.setBrightness(cfg.bright);
  lcd.fillScreen(TFT_WHITE);
  uiInit();             // allocates the render strip before Wi-Fi takes memory
  netInit();
  Serial.printf("[deck] ready, heap %u largest %u\n", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') { if (serialLine.length()) handleCommand(serialLine); serialLine = ""; }
    else if (serialLine.length() < 1500) serialLine += c;   // deck lines carry JSON
  }
  portalLoop();
  uiLoop();
  delay(5);
}
