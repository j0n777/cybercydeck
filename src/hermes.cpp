// Hermes Gadget Protocol v1 client (docs/protocol.md in Adolanium/hermes-gadget-sdk).
// Text gadget: no microphone. Replies, notices, cards and yes/no prompts show on the
// agent screen; Hermes can call the actions declared in hello.
#include "app.h"
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <mbedtls/base64.h>
#include <esp_random.h>

HermesState hermes;
static WebSocketsClient ws;
static uint8_t devKey[32];
static bool keyEnrolled = false;   // we have sent our key at least once
static volatile bool wantReplyYes = false, wantReplyNo = false;
static String pendingText;
static uint32_t lastState = 0;

static String b64(const uint8_t* d, size_t n) {
  size_t olen = 0;
  unsigned char out[96];
  mbedtls_base64_encode(out, sizeof(out), &olen, d, n);
  return String((const char*)out).substring(0, olen);
}

static void loadKey() {
  Preferences p;
  p.begin("hgkey", false);
  if (p.getBytesLength("key") == 32) {
    p.getBytes("key", devKey, 32);
    keyEnrolled = p.getBool("sent", false);
  } else {
    esp_fill_random(devKey, 32);
    p.putBytes("key", devKey, 32);
    p.putBool("sent", false);
    keyEnrolled = false;
  }
  p.end();
  uint8_t h[32];
  mbedtls_sha256(devKey, 32, h, 0);
  char hex[17];
  for (int i = 0; i < 8; i++) sprintf(hex + i * 2, "%02x", h[i]);
  hex[16] = 0;
  hermes.deviceId = String("hg-") + hex;
}

static void markEnrolled() {
  if (keyEnrolled) return;
  Preferences p;
  p.begin("hgkey", false);
  p.putBool("sent", true);
  p.end();
  keyEnrolled = true;
}

static void sendJson(JsonDocument& d) {
  String s;
  serializeJson(d, s);
  ws.sendTXT(s);
}

static void sendHello() {
  JsonDocument d;
  d["type"] = "hello";
  d["proto"] = 1;
  d["device_id"] = hermes.deviceId;
  d["name"] = PROJECT_NAME;
  d["board"] = BOARD_NAME;
  d["firmware"] = FW_VERSION;
  if (cfg.hermesToken.length()) d["token"] = cfg.hermesToken;
  JsonObject disp = d["caps"]["display"].to<JsonObject>();
  disp["width"] = 320; disp["height"] = 240; disp["color"] = true;
  disp["charset"] = "latin1";
  disp["text_cols"] = 40; disp["text_rows"] = 9;
  JsonArray in = d["caps"]["inputs"].to<JsonArray>();
  in.add("talk"); in.add("cancel");
  JsonArray acts = d["actions"].to<JsonArray>();
  {
    JsonObject a = acts.add<JsonObject>();
    a["name"] = "led.set";
    a["description"] = "Set the colour of the RGB status LED on the desk panel: red, green, blue, yellow, purple, cyan, white or off.";
    a["params"]["type"] = "object";
    a["params"]["properties"]["color"]["type"] = "string";
    a["params"]["required"].add("color");
  }
  {
    JsonObject a = acts.add<JsonObject>();
    a["name"] = "beep";
    a["description"] = "Play a short beep on the desk panel to get the owner's attention.";
    a["params"]["type"] = "object";
    a["params"]["properties"]["times"]["type"] = "integer";
  }
  {
    JsonObject a = acts.add<JsonObject>();
    a["name"] = "screen.show";
    a["description"] = "Switch the desk panel to a screen: home, btc, clima, redes, agenda, noticias, maquinas, pihole, casa, agente, jogos, deck, config.";
    a["params"]["type"] = "object";
    a["params"]["properties"]["screen"]["type"] = "string";
    a["params"]["required"].add("screen");
  }
  d["sensors"]["light_pct"] = lightPct();
  sendJson(d);
}

static void sendAuth(const String& nonce, bool enrolled) {
  JsonDocument d;
  d["type"] = "auth";
  if (!enrolled) {
    d["key"] = b64(devKey, 32);
  } else {
    String msg = "hermes-gadget/v1|" + hermes.deviceId + "|" + nonce;
    uint8_t mac[32];
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_hmac(md, devKey, 32, (const uint8_t*)msg.c_str(), msg.length(), mac);
    d["mac"] = b64(mac, 32);
  }
  sendJson(d);
}

static void actionResult(const String& id, bool ok, const char* key, const String& val, const char* err = nullptr) {
  JsonDocument d;
  d["type"] = "action.result";
  d["id"] = id;
  d["ok"] = ok;
  if (ok) d["result"][key] = val;
  else d["error"] = err ? err : "failed";
  sendJson(d);
}

static void handleAction(JsonDocument& m) {
  String id = m["id"] | "";
  String name = m["name"] | "";
  if (name == "led.set") {
    String c = m["args"]["color"] | "";
    if (!c.length()) { actionResult(id, false, "", "", "color is required"); return; }
    ledColorName(c);
    actionResult(id, true, "led", c);
  } else if (name == "beep") {
    int n = constrain((int)(m["args"]["times"] | 1), 1, 5);
    for (int i = 0; i < n; i++) { beep(); delay(120); }
    actionResult(id, true, "beeps", String(n));
  } else if (name == "screen.show") {
    String s = m["args"]["screen"] | "";
    int sc = screenFromName(s);
    if (sc < 0) { actionResult(id, false, "", "", "unknown screen"); return; }
    uiGoto(sc);
    actionResult(id, true, "screen", s);
  } else {
    actionResult(id, false, "", "", "unknown action");
  }
}

static void onMessage(const char* txt, size_t len) {
  JsonDocument m;
  if (deserializeJson(m, txt, len)) return;
  String type = m["type"] | "";
  if (type == "challenge") {
    bool enrolled = m["enrolled"] | false;
    sendAuth(String((const char*)(m["nonce"] | "")), enrolled);
    if (!enrolled) markEnrolled();
  } else if (type == "welcome") {
    bool paired = m["paired"] | false;
    hermes.status = paired ? 3 : 2;
    hermes.err = "";
    Serial.printf("[hermes] welcome paired=%d\n", paired);
  } else if (type == "pairing") {
    hermes.status = 2;
    hermes.pairCode = String((const char*)(m["code"] | ""));
    Serial.printf("[hermes] pairing code %s\n", hermes.pairCode.c_str());
  } else if (type == "paired") {
    hermes.status = 3; hermes.pairCode = "";
    beep(2600, 80);
  } else if (type == "unpaired") {
    hermes.status = 2;
  } else if (type == "reply" || type == "reply.delta") {
    hermes.lastTitle = "Agente";
    hermes.lastReply = String((const char*)(m["text"] | ""));
  } else if (type == "status") {
    hermes.statusLine = String((const char*)(m["text"] | ""));
  } else if (type == "turn.end") {
    hermes.statusLine = "";
  } else if (type == "notice") {
    hermes.notice = String((const char*)(m["text"] | ""));
    hermes.noticeUntil = millis() + 1000UL * (int)(m["ttl_s"] | 8);
    notifyAdd(N_AGENTE, "Agente", hermes.notice);
  } else if (type == "display") {
    hermes.lastTitle = String((const char*)(m["title"] | "Agente"));
    hermes.lastReply = String((const char*)(m["body"] | ""));
    hermes.notice = hermes.lastTitle + ": " + hermes.lastReply;
    hermes.noticeUntil = millis() + 1000UL * max(5, (int)(m["ttl_s"] | 10));
    notifyAdd(N_AGENTE, hermes.lastTitle, hermes.lastReply);
  } else if (type == "prompt") {
    // a question from Hermes (dangerous command, /new, model switch...): it waits on the agent
    // screen; the home screen and the LED point to it, nothing jumps screens on its own
    hermes.promptId = String((const char*)(m["id"] | ""));
    hermes.promptTitle = String((const char*)(m["title"] | "Confirmar"));
    hermes.promptText = String((const char*)(m["text"] | ""));
    int ttl = m["ttl_s"] | 0;
    hermes.promptUntil = ttl > 0 ? millis() + 1000UL * ttl : 0;
    hermes.notice = "Agente pede aprovação: " + hermes.promptTitle;
    hermes.noticeUntil = millis() + 8000;
    notifyAdd(N_APROV, hermes.promptTitle, hermes.promptText);
    beep(1800, 90);
  } else if (type == "prompt.close") {
    if (hermes.promptId == String((const char*)(m["id"] | ""))) { hermes.promptId = ""; hermes.promptUntil = 0; }
  } else if (type == "action") {
    handleAction(m);
  } else if (type == "ping") {
    JsonDocument d;
    d["type"] = "pong";
    d["ts"] = m["ts"];
    sendJson(d);
  } else if (type == "error") {
    hermes.err = String((const char*)(m["message"] | m["code"] | "erro"));
    hermes.status = 4;
    Serial.printf("[hermes] error %s\n", hermes.err.c_str());
  }
  uiRequestRedraw();
}

static void onEvent(WStype_t t, uint8_t* payload, size_t len) {
  switch (t) {
    case WStype_CONNECTED:
      Serial.println("[hermes] connected");
      hermes.status = 1;
      sendHello();
      break;
    case WStype_DISCONNECTED:
      if (hermes.status != 4) hermes.status = 1;
      break;
    case WStype_TEXT:
      onMessage((const char*)payload, len);
      break;
    default:
      break;
  }
}

void hermesBegin() {
  loadKey();
  String u = cfg.hermesUrl;
  bool ssl = u.startsWith("wss://");
  u.replace("wss://", ""); u.replace("ws://", "");
  int slash = u.indexOf('/');
  String hostport = slash >= 0 ? u.substring(0, slash) : u;
  String path = slash >= 0 ? u.substring(slash) : "/gadget";
  int colon = hostport.indexOf(':');
  String host = colon >= 0 ? hostport.substring(0, colon) : hostport;
  int port = colon >= 0 ? hostport.substring(colon + 1).toInt() : (ssl ? 443 : 8765);
  Serial.printf("[hermes] %s host=%s port=%d path=%s id=%s\n", ssl ? "wss" : "ws", host.c_str(), port, path.c_str(), hermes.deviceId.c_str());
  if (ssl) ws.beginSSL(host.c_str(), port, path.c_str(), "", "hermes-gadget.v1");
  else ws.begin(host.c_str(), port, path.c_str(), "hermes-gadget.v1");
  ws.onEvent(onEvent);
  ws.setReconnectInterval(5000);
  hermes.status = 1;
}

// test hook: "hermes {json}" on the USB serial feeds a server frame (prompt, display, notice...)
void hermesInject(const String& json) { onMessage(json.c_str(), json.length()); }

void hermesLoop() {
  ws.loop();
  if (hermes.promptId.length() && hermes.promptUntil && (int32_t)(millis() - hermes.promptUntil) > 0) {
    hermes.promptId = ""; hermes.promptUntil = 0;   // expired: Hermes withdraws it and treats it as no answer
    uiRequestRedraw();
  }
  if (hermes.status >= 2 && hermes.status <= 3) {
    if (wantReplyYes || wantReplyNo) {
      JsonDocument d;
      d["type"] = "prompt.reply";
      d["id"] = hermes.promptId;
      d["answer"] = wantReplyYes ? "yes" : "no";
      sendJson(d);
      wantReplyYes = wantReplyNo = false;
      hermes.promptId = ""; hermes.promptUntil = 0;
      uiRequestRedraw();
    }
    if (pendingText.length()) {
      JsonDocument d;
      d["type"] = "text";
      d["id"] = String("t") + millis();
      d["text"] = pendingText;
      sendJson(d);
      pendingText = "";
    }
    if (millis() - lastState > 300000UL) {
      lastState = millis();
      JsonDocument d;
      d["type"] = "state";
      d["sensors"]["light_pct"] = lightPct();
      sendJson(d);
    }
  }
}

void hermesSendPromptReply(bool yes) { if (yes) wantReplyYes = true; else wantReplyNo = true; }
void hermesSendText(const String& t) { pendingText = t; }
