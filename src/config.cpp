#include "app.h"
#include <Preferences.h>
#include <esp_random.h>

Config cfg;
static Preferences prefs;

static String gs(const char* k, const char* d = "") { return prefs.getString(k, d); }

void configLoad() {
  prefs.begin("ydeck", true);   // namespace kept from the first name, so saved settings survive updates
  cfg.ssid = gs("ssid");
  cfg.wpass = gs("wpass");
  cfg.apPass = gs("appass");
  cfg.ssid2 = gs("ssid2"); cfg.wpass2 = gs("wpass2");
  cfg.ssid3 = gs("ssid3"); cfg.wpass3 = gs("wpass3");
  cfg.nome = gs("nome");
  cfg.n8nUrl = gs("n8n");
  cfg.feedUrl = gs("feed");
  cfg.phUrl = gs("phurl"); cfg.phPass = gs("phpass");
  cfg.mods = prefs.getUShort("mods", 0xF3F);
  bool migrate = prefs.getUChar("modsv", 0) < 3;
  cfg.feedToken = gs("feedtok");
  cfg.icalUrl = gs("ical");
  cfg.sshHost = gs("sshhost"); cfg.sshUser = gs("sshuser"); cfg.sshPass = gs("sshpass"); cfg.sshFp = gs("sshfp");
  cfg.sshPort = prefs.getInt("sshport", 22);
  cfg.alertBeep = prefs.getBool("abeep", false);
  cfg.nightDim = prefs.getBool("night", true);
  cfg.haUrl = gs("haurl"); cfg.haToken = gs("hatok");
  const char* defNames[4] = {"Luz da sala", "Ventilador", "Bancada", "Pi-hole"};
  for (int i = 0; i < 4; i++) {
    char k[8];
    snprintf(k, sizeof(k), "hae%d", i); cfg.haEnt[i] = gs(k);
    snprintf(k, sizeof(k), "han%d", i); cfg.haName[i] = gs(k, defNames[i]);
  }
  cfg.hermesUrl = gs("hermes");
  cfg.hermesToken = gs("htok");
  cfg.meshIp = gs("mesh");
  cfg.city = gs("city");
  cfg.lat = prefs.getFloat("lat", NAN);
  cfg.lon = prefs.getFloat("lon", NAN);
  cfg.btcEvery = prefs.getInt("btcev", 60);
  cfg.bright = prefs.getInt("bright", 180);
  cfg.invert = prefs.getBool("inv", false);
  cfg.theme = prefs.getUChar("theme", 0);
  cfg.homeItems = prefs.getUInt("home", 0x621);
  cfg.anim = prefs.getBool("anim", true);
  cfg.apagao = prefs.getBool("apagao", true);
  cfg.rgbOrder = prefs.getBool("rgb", false);
  cfg.calOk = prefs.getBool("calok", false);
  cfg.calSwap = prefs.getBool("calsw", false);
  cfg.calAx = prefs.getFloat("calax", 0); cfg.calBx = prefs.getFloat("calbx", 0);
  cfg.calAy = prefs.getFloat("calay", 0); cfg.calBy = prefs.getFloat("calby", 0);
  cfg.snakeBest = prefs.getInt("snake", 0);
  prefs.end();
  if (migrate) {
    // 0.3: new modules on; IR and Mesh off until the hardware exists
    cfg.mods |= (M_AGENDA | M_NEWS | M_MAQ);
    cfg.mods &= ~(M_IR | M_MESH);
    configSave();
  }
  if (cfg.apPass.length() != 8) {
    // 8 random digits: password of the setup network and PIN of the web page
    char b[9];
    for (int i = 0; i < 8; i++) b[i] = '0' + (esp_random() % 10);
    b[8] = 0;
    cfg.apPass = b;
    configSave();
  }
}

void configSave() {
  prefs.begin("ydeck", false);
  prefs.putString("ssid", cfg.ssid);
  prefs.putString("wpass", cfg.wpass);
  prefs.putString("appass", cfg.apPass);
  prefs.putString("ssid2", cfg.ssid2); prefs.putString("wpass2", cfg.wpass2);
  prefs.putString("ssid3", cfg.ssid3); prefs.putString("wpass3", cfg.wpass3);
  prefs.putString("nome", cfg.nome);
  prefs.putString("n8n", cfg.n8nUrl);
  prefs.putString("feed", cfg.feedUrl);
  prefs.putString("phurl", cfg.phUrl); prefs.putString("phpass", cfg.phPass);
  prefs.putUShort("mods", cfg.mods);
  prefs.putUChar("modsv", 3);
  prefs.putString("feedtok", cfg.feedToken);
  prefs.putString("ical", cfg.icalUrl);
  prefs.putString("sshhost", cfg.sshHost); prefs.putString("sshuser", cfg.sshUser); prefs.putString("sshpass", cfg.sshPass);
  prefs.putString("sshfp", cfg.sshFp); prefs.putInt("sshport", cfg.sshPort);
  prefs.putBool("abeep", cfg.alertBeep);
  prefs.putBool("night", cfg.nightDim);
  prefs.putString("haurl", cfg.haUrl); prefs.putString("hatok", cfg.haToken);
  for (int i = 0; i < 4; i++) {
    char k[8];
    snprintf(k, sizeof(k), "hae%d", i); prefs.putString(k, cfg.haEnt[i]);
    snprintf(k, sizeof(k), "han%d", i); prefs.putString(k, cfg.haName[i]);
  }
  prefs.putString("hermes", cfg.hermesUrl);
  prefs.putString("htok", cfg.hermesToken);
  prefs.putString("mesh", cfg.meshIp);
  prefs.putString("city", cfg.city);
  prefs.putFloat("lat", cfg.lat); prefs.putFloat("lon", cfg.lon);
  prefs.putInt("btcev", cfg.btcEvery);
  prefs.putInt("bright", cfg.bright);
  prefs.putBool("inv", cfg.invert);
  prefs.putUChar("theme", cfg.theme);
  prefs.putUInt("home", cfg.homeItems);
  prefs.putBool("anim", cfg.anim);
  prefs.putBool("apagao", cfg.apagao);
  prefs.putBool("rgb", cfg.rgbOrder);
  prefs.putBool("calok", cfg.calOk);
  prefs.putBool("calsw", cfg.calSwap);
  prefs.putFloat("calax", cfg.calAx); prefs.putFloat("calbx", cfg.calBx);
  prefs.putFloat("calay", cfg.calAy); prefs.putFloat("calby", cfg.calBy);
  prefs.putInt("snake", cfg.snakeBest);
  prefs.end();
}

void configFactoryReset() {
  prefs.begin("ydeck", false);
  prefs.clear();
  prefs.end();
  // the Hermes device key lives in its own namespace and is cleared too
  prefs.begin("hgkey", false);
  prefs.clear();
  prefs.end();
}
