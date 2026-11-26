#pragma once
#include <Arduino.h>
#include "board.h"
#include "vt.h"

#define FW_VERSION "0.3.0"
#define PROJECT_NAME "CyberCYDeck"
#define AP_SSID "CyberCYDeck"
#define HOSTNAME "cybercydeck"
#define BOARD_NAME "esp32-2432s028"

// ---------- persisted config (NVS) ----------
struct Config {
  String ssid, wpass;         // Wi-Fi 1
  String ssid2, wpass2, ssid3, wpass3;  // Wi-Fi 2 and 3 (joins the strongest one in range)
  String apPass;              // password of the setup network (AP_SSID) (also the web PIN)
  String nome;                // name shown in the greeting (optional)
  String n8nUrl;              // followers endpoint (JSON: total, instagram, youtube...)
  String feedUrl;             // summary feed (JSON: saude, agenda, manchetes, maquinas, ia)
  String phUrl, phPass;       // Pi-hole v6 (local network)
  uint16_t mods = 0xF3F;      // enabled modules, see M_* below (IR and Mesh off by default)
  String feedToken;           // sent as "Authorization: Bearer <token>" to the feed
  String icalUrl;             // calendar .ics link (Google: "secret address in iCal format"); replaces the feed agenda
  bool alertBeep = false;     // beep on alerts (never at night)
  bool nightDim = true;       // dim the backlight from 23h to 7h
  String haUrl, haToken;
  String haEnt[4], haName[4];
  String hermesUrl;           // ws://host:8765/gadget
  String hermesToken;         // optional GADGET_ACCESS_TOKEN
  String meshIp;
  String city;
  float lat = NAN, lon = NAN;  // geocoded from city
  int btcEvery = 60;          // seconds
  int bright = 180;           // 0..255
  bool invert = false, rgbOrder = false;
  uint32_t homeItems = 0x621;   // home slots, one item per nibble in order (see HI_* in ui.cpp)
  uint8_t theme = 0;          // 0 claro, 1 terminal (escuro), 2 terminal claro
  bool anim = true;           // terminal effects: boot log, cursor, scan transitions
  bool apagao = true;         // blackout mode: switch to the offline screen when the internet drops
  bool calOk = false;
  bool calSwap = false;
  float calAx = 0, calBx = 0, calAy = 0, calBy = 0;
  int snakeBest = 0;
};
extern Config cfg;
// modules that can be switched on/off (Config > Módulos or the web form)
enum { M_BTC = 1, M_CLIMA = 2, M_REDES = 4, M_CASA = 8, M_AGENTE = 16, M_JOGOS = 32, M_IR = 64, M_MESH = 128, M_PIHOLE = 256,
       M_AGENDA = 512, M_NEWS = 1024, M_MAQ = 2048, M_COUNT = 12 };
#define MOD_ON(m) ((cfg.mods & (m)) != 0)
void configLoad();
void configSave();
void configFactoryReset();

// ---------- live data ----------
struct LiveData {
  bool btcOk = false;
  float btc = 0, btcChg = 0, eth = 0, ethChg = 0, sol = 0, solChg = 0;
  int feeSlow = 0, feeStd = 0, feeFast = 0;
  int fng = -1; String fngLabel;
  uint32_t btcAt = 0;          // epoch seconds of last fetch
  bool wxOk = false;
  float wxTemp = 0; int wxHum = 0; float wxWind = 0; int wxCode = 0;
  String wxPlace; bool wxByIp = false;   // place name; true when located by the IP address
  float dMax[4] = {0}, dMin[4] = {0}; int dCode[4] = {0}; String dDay[4];
  uint32_t wxAt = 0;
  bool socOk = false;
  long socTotal = -1, socYt = -1, socIg = -1, socTt = -1, socX = -1, socTg = -1, socTh = -1, socFb = -1;
  uint32_t socAt = 0;
  bool haOk = false;
  int haState[4] = {-1, -1, -1, -1};  // -1 unknown, 0 off, 1 on
  String haErr;
  bool phOk = false;
  long phTotal = -1, phBlocked = -1, phDomains = -1; float phPct = 0; int phClients = -1;
  String phErr; uint32_t phAt = 0;
  // summary feed published by your server (see docs/feed.md)
  bool plOk = false; uint32_t plAt = 0; String plErr; String saude;   // saude: ok | alerta | critico
  int agN = 0; String agT[3]; uint32_t agQ[3] = {0}; bool agDia[3] = {false};
  bool icOk = false; uint32_t icAt = 0; String icErr;   // agenda read from the iCal link
  int nwN = 0; String nwT[5], nwF[5];
  int mqN = 0; String mqNome[5]; int mqCpu[5] = {0}, mqRam[5] = {0}, mqTemp[5] = {0}, mqDisk[5] = {0}; uint32_t mqAge[5] = {0}; bool mqOn[5] = {false};
  float iaCusto = -1; double iaTokens = -1; int iaN = 0; String iaNome[4]; float iaC[4] = {0};
  // cyberdeck host (Orange Pi / Raspberry Pi) talking over the USB serial, see docs/deck-protocol.md
  uint32_t dkAt = 0;                 // millis() of the last "deck" line, 0 = never
  String dkHost;
  int dkBat = -1; float dkV = 0; bool dkChg = false;
  int dkCpu = -1, dkRam = -1, dkTemp = -1; uint32_t dkUp = 0;
  String dkWifi, dkIp; int dkRssi = 0;
  String dkLte; int dkSig = -1;
  bool dkFix = false; int dkSats = -1;
  int dkMeshN = -1; int dkMsgN = 0; String dkMsgDe[3], dkMsgTxt[3]; uint32_t dkMsgT[3] = {0};
  String dkAlert;
  // radio: HF propagation (N0NBH, hamqsl.com) and the next ISS pass (TLE from CelesTrak, SGP4 on the board)
  bool rdOk = false; uint32_t rdAt = 0;
  int rdSfi = -1, rdA = -1, rdK = -1, rdSpots = -1;
  String rdXray, rdGeo, rdNoise;
  uint8_t rdBand[2][4] = {{0}};      // [day, night][80-40, 30-20, 17-15, 12-10]: 0 ?, 1 ruim, 2 regular, 3 boa
  bool issOk = false; uint32_t issAos = 0, issLos = 0; int issMaxEl = -1, issAzAos = -1, issAzLos = -1;
  String rdErr;
};
extern LiveData live;
extern SemaphoreHandle_t dataLock;

// ---------- Hermes gadget state ----------
struct HermesState {
  int status = 0;              // 0 off, 1 connecting, 2 pairing, 3 paired, 4 error
  String pairCode;
  String lastReply;            // last reply / display card body
  String lastTitle;
  String statusLine;           // live working-state phrase
  String promptId, promptTitle, promptText;
  uint32_t promptUntil = 0;    // millis() when the question expires, 0 = no expiry
  String notice; uint32_t noticeUntil = 0;
  String err;
  String deviceId;
};
extern HermesState hermes;
void hermesSendPromptReply(bool yes);
void hermesSendText(const String& t);
void hermesInject(const String& json);

// ---------- notifications (screen "Avisos") ----------
enum { N_APROV, N_AGENTE, N_MAQ, N_REDE, N_DECK };
struct Notif { uint8_t kind; String title, text; uint32_t at; };   // at: epoch seconds (0 if the clock is unknown)
#define NOTIF_MAX 8
extern Notif notifs[NOTIF_MAX];       // newest first
extern volatile int notifN, notifUnread;
void notifyAdd(uint8_t kind, const String& title, const String& text);   // never call while holding dataLock

// ---------- log readable at /log ----------
void logf(const char* fmt, ...);
String logDump();

// ---------- SSH terminal ----------
extern Vt vt;                          // the terminal screen (guarded by dataLock)
extern volatile int sshState;          // 0 off, 1 connecting, 2 open, 3 closed/failed
extern String sshMsg;                  // "user@host" while a session is open
void termOpen();                       // the terminal screen opened: local prompt ("cyd$"), type ssh user@host
void termInput(const char* s, int n);  // keys from the touch keyboard
void sshStop();

// ---------- internet check (blackout mode) ----------
extern volatile bool netOnline;        // a public address answered in the last check
extern volatile uint32_t offlineSince; // millis() of the first failed check, 0 = online
void netRequestRadio();                // fetch propagation + ISS pass now (opening the radio screen)

// ---------- screens ----------
enum Screen { S_HOME, S_BTC, S_CLIMA, S_REDES, S_CASA, S_AGENTE, S_JOGOS, S_SNAKE, S_IR, S_MESH, S_PIHOLE, S_AGENDA, S_NEWS, S_MAQ, S_APPS, S_MODS, S_CONFIG, S_QR, S_CAL, S_DECK, S_WIFI, S_WIFISCAN, S_KEYB, S_WIFIREP, S_HOMECFG, S_AVISOS, S_RADIO, S_APAGAO, S_SSH, S_TANKS, S_COUNT };
extern volatile int curScreen;
void uiInit();
void uiGoto(int s);
void uiRequestRedraw();
void uiLoop();
void uiTap(int x, int y);
bool uiScreenshot(Stream& out);
void uiApplyTheme();
const char* screenName(int s);
int screenFromName(const String& n);

// ---------- network ----------
extern volatile bool wifiUp, apMode;
extern String ipStr;
void netInit();
void netLoop();              // runs in its own task
void portalLoop();           // runs in the UI task
void netRequestHaToggle(int i);
void netRequestRefresh();
void netRequestPlaca();
// on-device Wi-Fi setup (Config › Wi-Fi)
struct WifiNet { String ssid; int rssi; bool open; };
extern WifiNet wifiNets[16];
extern volatile int wifiNetsN;          // results of the last scan
extern volatile bool wifiScanBusy;
extern volatile int wifiJoin;           // 0 idle, 1 connecting, 2 connected, 3 failed
extern String wifiJoinSsid, wifiJoinErr;
void netRequestScan();
void netConnectNow(const String& ssid, const String& pass);
uint32_t parseIso(const char* s);   // ISO-8601 -> epoch seconds (0 if invalid)

// ---------- cyberdeck host link (USB serial) ----------
void deckLine(const String& json);  // handles "deck {json}" from the host
bool deckFresh();                    // host sent data in the last 15 s

// ---------- hw helpers ----------
extern LGFX lcd;
void ledSet(uint8_t r, uint8_t g, uint8_t b);
void ledColorName(const String& c);
void beep(int freq = 2200, int ms = 120);
int lightPct();

// ---------- touch ----------
bool touchRaw(int& rx, int& ry);
void touchDump(Stream& o);
bool touchRead(int& x, int& y);   // calibrated screen coords
