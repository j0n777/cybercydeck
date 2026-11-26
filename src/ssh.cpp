// Terminal: a small local prompt ("cyd$") where you type `ssh user@host` like on a computer.
// The password is asked on the spot and never stored. Each server's key is remembered on the
// first connection after you confirm it (like ~/.ssh/known_hosts); a different key later is refused.
// libssh runs in its own task (it needs a large stack); while a session is open the network task
// pauses its HTTPS fetches so the session has the memory it needs.
#include "app.h"
#include <Preferences.h>
#include "libssh_esp32.h"
#include <libssh/libssh.h>

Vt vt;
volatile int sshState = 0;           // 0 local prompt, 1 connecting, 2 session open
String sshMsg;
static String outBuf;                // keys for the remote side
static portMUX_TYPE outMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool wantClose = false;
static bool libsshReady = false, opened = false;

// local prompt state
enum { L_CMD, L_PASS, L_TRUST };
static volatile int lmode = L_CMD;
static String line, hist[5];
static int histN = 0, histPos = -1;
static String tHost, tUser; static int tPort = 22;
static char tPass[96];
static volatile int trustAns = 0;    // 0 waiting, 1 yes, 2 no

static void out(const String& s) {   // write to the screen (any task)
  xSemaphoreTake(dataLock, portMAX_DELAY);
  vt.feed(s.c_str(), s.length());
  xSemaphoreGive(dataLock);
  uiRequestRedraw();
}
static void prompt() { out("\x1b[1;32mcyd\x1b[0m$ "); }

// ---------- known hosts: "host:port SHA256:..." one per line, in the board's settings ----------
static String khLoad() { Preferences p; p.begin("ydeck", true); String s = p.getString("sshkh", ""); p.end(); return s; }
static void khSave(const String& s) { Preferences p; p.begin("ydeck", false); p.putString("sshkh", s); p.end(); }
static String khFind(const String& id) {
  String all = "\n" + khLoad();
  int a = all.indexOf("\n" + id + " ");
  if (a < 0) return "";
  a += id.length() + 2;
  int e = all.indexOf('\n', a);
  return all.substring(a, e < 0 ? all.length() : e);
}
static int khForget(const String& prefix) {   // drops every line starting with prefix; returns how many
  String all = khLoad(), keep; int n = 0, p = 0;
  while (p < (int)all.length()) {
    int e = all.indexOf('\n', p); if (e < 0) e = all.length();
    String l = all.substring(p, e);
    if (l.length()) { if (l.startsWith(prefix)) n++; else keep += l + "\n"; }
    p = e + 1;
  }
  khSave(keep);
  return n;
}
static void khAdd(const String& id, const String& fp) {
  khForget(id + " ");
  String all = khLoad();
  int lines = 0; for (unsigned i = 0; i < all.length(); i++) if (all[i] == '\n') lines++;
  while (lines >= 12) { all = all.substring(all.indexOf('\n') + 1); lines--; }   // keep the 12 newest
  khSave(all + id + " " + fp + "\n");
}

// ---------- the session ----------
static void say(const String& m) { logf("[ssh] %s", m.c_str()); out("\r\n\x1b[33m" + m + "\x1b[0m"); }

static void sshTask(void*) {
  if (!libsshReady) { libssh_begin(); libsshReady = true; }
  ssh_session s = ssh_new();
  ssh_channel ch = nullptr;
  ssh_key srv = nullptr;
  unsigned char* hash = nullptr; size_t hlen = 0;
  char* fp = nullptr;
  String id = tHost + ":" + tPort, known;
  long timeout = 12;
  ssh_options_set(s, SSH_OPTIONS_HOST, tHost.c_str());
  ssh_options_set(s, SSH_OPTIONS_PORT, &tPort);
  ssh_options_set(s, SSH_OPTIONS_USER, tUser.c_str());
  ssh_options_set(s, SSH_OPTIONS_TIMEOUT, &timeout);
  if (ssh_connect(s) != SSH_OK) { say(String("não conectou: ") + ssh_get_error(s)); goto done; }

  if (ssh_get_server_publickey(s, &srv) != SSH_OK || ssh_get_publickey_hash(srv, SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen) != 0) {
    say("não consegui ler a chave do servidor"); goto done;
  }
  fp = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hlen);
  known = khFind(id);
  if (!known.length()) {
    out(String("\r\nPrimeira conexão com ") + id + ".\r\nIdentidade do servidor: " + fp + "\r\nConfiar neste servidor? (s/n) ");
    trustAns = 0; lmode = L_TRUST;
    uint32_t t0 = millis();
    while (!trustAns && !wantClose && millis() - t0 < 90000) vTaskDelay(pdMS_TO_TICKS(50));
    lmode = L_CMD;
    if (trustAns != 1) { say("cancelado"); goto done; }
    khAdd(id, fp);
  } else if (known != fp) {
    say(String("ATENÇÃO: a identidade de ") + id + " MUDOU. Pode ser um ataque. Se foi você que trocou o servidor, rode: esquecer " + tHost);
    goto done;
  }

  {
    int rc = ssh_userauth_password(s, nullptr, tPass);
    memset(tPass, 0, sizeof(tPass));                         // the password does not outlive the login
    if (rc != SSH_AUTH_SUCCESS) { say("login recusado: confira usuário e senha"); goto done; }
  }
  ch = ssh_channel_new(s);
  if (!ch || ssh_channel_open_session(ch) != SSH_OK || ssh_channel_request_pty_size(ch, "xterm", VT_COLS, VT_ROWS) != SSH_OK ||
      ssh_channel_request_shell(ch) != SSH_OK) { say(String("o servidor recusou o terminal: ") + ssh_get_error(s)); goto done; }
  xSemaphoreTake(dataLock, portMAX_DELAY); sshMsg = tUser + "@" + tHost; sshState = 2; xSemaphoreGive(dataLock);
  out("\r\n");
  logf("[ssh] sessão aberta em %s", id.c_str());
  {
    static char buf[512];
    uint32_t idle = millis();
    while (!wantClose) {
      int n = ssh_channel_read_nonblocking(ch, buf, sizeof(buf), 0);
      if (n == SSH_ERROR) { say("conexão caiu"); break; }
      if (n > 0) {
        xSemaphoreTake(dataLock, portMAX_DELAY);
        vt.feed(buf, n);
        String rep = vt.replyLen ? String(vt.reply).substring(0, vt.replyLen) : String("");
        vt.replyLen = 0;
        xSemaphoreGive(dataLock);
        if (rep.length()) ssh_channel_write(ch, rep.c_str(), rep.length());
        uiRequestRedraw();
        idle = millis();
      }
      if (ssh_channel_is_eof(ch) || ssh_channel_is_closed(ch)) break;
      String send;
      portENTER_CRITICAL(&outMux); send = outBuf; outBuf = ""; portEXIT_CRITICAL(&outMux);
      if (send.length()) { ssh_channel_write(ch, send.c_str(), send.length()); idle = millis(); }
      vTaskDelay(pdMS_TO_TICKS(n > 0 ? 2 : (millis() - idle < 2000 ? 15 : 40)));
    }
  }

done:
  memset(tPass, 0, sizeof(tPass));
  if (ch) { ssh_channel_close(ch); ssh_channel_free(ch); }
  if (fp) ssh_string_free_char(fp);
  if (hash) ssh_clean_pubkey_hash(&hash);
  if (srv) ssh_key_free(srv);
  ssh_disconnect(s); ssh_free(s);
  wantClose = false;
  portENTER_CRITICAL(&outMux); outBuf = ""; portEXIT_CRITICAL(&outMux);
  xSemaphoreTake(dataLock, portMAX_DELAY); sshState = 0; sshMsg = ""; xSemaphoreGive(dataLock);
  out("\x1b[0m\r\n[conexão encerrada]\r\n");
  prompt();
  vTaskDelete(nullptr);
}

static void startSession() {
  if (!wifiUp) { say("sem Wi-Fi"); out("\r\n"); prompt(); return; }
  if (ESP.getMaxAllocHeap() < 40000) { say("memória livre insuficiente agora; tente de novo em instantes"); out("\r\n"); prompt(); return; }
  wantClose = false;
  sshState = 1;
  out(String("conectando em ") + tHost + ":" + tPort + "…");
  // 32 KB of stack: the library examples use 50 KB for more than a shell
  if (xTaskCreatePinnedToCore(sshTask, "ssh", 32768, nullptr, 1, nullptr, 0) != pdPASS) {
    sshState = 0; memset(tPass, 0, sizeof(tPass)); say("não consegui abrir a tarefa do terminal"); out("\r\n"); prompt();
  }
}

// ---------- local prompt ----------
static void run(String cmd) {
  cmd.trim();
  if (cmd.length()) { for (int i = 4; i > 0; i--) hist[i] = hist[i - 1]; hist[0] = cmd; if (histN < 5) histN++; }
  if (!cmd.length()) { prompt(); return; }
  if (cmd == "ajuda" || cmd == "help") {
    out("ssh usuario@host          conecta (porta 22)\r\n"
        "ssh -p 2222 usuario@host  outra porta\r\n"
        "hosts                     servidores conhecidos\r\n"
        "esquecer host             esquece a identidade de um\r\n"
        "limpar                    limpa a tela\r\n");
    prompt(); return;
  }
  if (cmd == "limpar" || cmd == "clear") { out("\x1b[2J\x1b[H"); prompt(); return; }
  if (cmd == "hosts") {
    String all = khLoad();
    if (!all.length()) out("nenhum servidor conhecido ainda\r\n");
    else { all.replace("\n", "\r\n"); out(all); }
    prompt(); return;
  }
  if (cmd.startsWith("esquecer ")) {
    String h = cmd.substring(9); h.trim();
    out(khForget(h + ":") ? "esquecido\r\n" : "não estava na lista\r\n");
    prompt(); return;
  }
  if (cmd.startsWith("ssh ") || cmd == "ssh") {
    int port = 22; String target;
    String rest = cmd.substring(3); rest.trim();
    while (rest.length()) {
      int sp = rest.indexOf(' ');
      String tok = sp < 0 ? rest : rest.substring(0, sp);
      rest = sp < 0 ? "" : rest.substring(sp + 1); rest.trim();
      if (tok == "-p" && rest.length()) {
        int sp2 = rest.indexOf(' ');
        port = (sp2 < 0 ? rest : rest.substring(0, sp2)).toInt();
        rest = sp2 < 0 ? "" : rest.substring(sp2 + 1); rest.trim();
      } else if (tok.startsWith("-p")) port = tok.substring(2).toInt();
      else target = tok;
    }
    int at = target.indexOf('@');
    if (at <= 0 || at == (int)target.length() - 1 || port <= 0 || port > 65535) { out("uso: ssh [-p porta] usuario@host\r\n"); prompt(); return; }
    tUser = target.substring(0, at); tHost = target.substring(at + 1); tPort = port;
    out(tUser + "@" + tHost + " senha: ");
    lmode = L_PASS; line = "";
    return;
  }
  out(cmd + ": comando não encontrado (digite ajuda)\r\n");
  prompt();
}

void termOpen() {
  if (opened) return;
  opened = true;
  xSemaphoreTake(dataLock, portMAX_DELAY); vt.reset(); xSemaphoreGive(dataLock);
  out("CyberCYDeck terminal. Digite ssh usuario@host (ou ajuda)\r\n");
  prompt();
}

static void replaceLine(const String& s) {   // history keys: erase what was typed, show the old command
  String back; for (unsigned i = 0; i < line.length(); i++) back += "\b \b";
  line = s; out(back + s);
}

void termInput(const char* s, int n) {
  if (sshState == 2) {
    portENTER_CRITICAL(&outMux);
    if (outBuf.length() < 512) outBuf.concat(s, n);
    portEXIT_CRITICAL(&outMux);
    return;
  }
  if (lmode == L_TRUST) {
    if (n == 1 && (s[0] == 's' || s[0] == 'S' || s[0] == 'y')) { out("s\r\n"); trustAns = 1; }
    else if (n == 1 && (s[0] == 'n' || s[0] == 'N' || s[0] == 3)) { out("n\r\n"); trustAns = 2; }
    return;
  }
  if (sshState == 1) return;   // connecting
  if (n == 3 && s[0] == 0x1b && (s[2] == 'A' || s[2] == 'B') && lmode == L_CMD) {   // history
    if (s[2] == 'A' && histPos + 1 < histN) histPos++;
    else if (s[2] == 'B' && histPos >= 0) histPos--;
    replaceLine(histPos >= 0 ? hist[histPos] : String(""));
    return;
  }
  for (int i = 0; i < n; i++) {
    char c = s[i];
    if (c == '\r') {
      histPos = -1;
      if (lmode == L_PASS) {
        strlcpy(tPass, line.c_str(), sizeof(tPass));
        for (unsigned k = 0; k < line.length(); k++) line[k] = 0;
        line = ""; lmode = L_CMD; out("\r\n");
        startSession();
      } else { String cmd = line; line = ""; out("\r\n"); run(cmd); }
    } else if (c == 0x7f || c == '\b') {
      if (line.length()) { line.remove(line.length() - 1); if (lmode == L_CMD) out("\b \b"); }
    } else if (c == 3) {   // ctrl+c
      line = ""; lmode = L_CMD; out("^C\r\n"); prompt();
    } else if ((uint8_t)c >= 0x20 && c != 0x1b) {
      if (line.length() < 90) { line += c; if (lmode == L_CMD) out(String(c)); }   // the password is not shown
    }
  }
}

void sshStop() { if (sshState == 1 || sshState == 2) wantClose = true; }
