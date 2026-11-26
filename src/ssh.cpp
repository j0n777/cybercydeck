// SSH terminal: one session at a time, in its own task (libssh needs a large stack: 32 KB here,
// the library examples use 50 KB for more than a shell).
// The server's key is pinned on the first connection (trust on first use); a different key later
// blocks the connection until it is forgotten in the portal. While a session is open, the
// network task pauses its HTTPS fetches so the session has the memory it needs.
#include "app.h"
#include "libssh_esp32.h"
#include <libssh/libssh.h>

Vt vt;
volatile int sshState = 0;           // 0 off, 1 connecting, 2 open, 3 closed/failed
String sshMsg;                       // last status or error, shown on the screen
static String outBuf;                // keys typed on the screen, waiting to be sent
static portMUX_TYPE outMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool wantClose = false;
static bool libsshReady = false;

void sshSend(const char* s, int n) {
  portENTER_CRITICAL(&outMux);
  if (outBuf.length() < 512) outBuf.concat(s, n);
  portEXIT_CRITICAL(&outMux);
}
bool sshConfigured() { return cfg.sshHost.length() && cfg.sshUser.length() && cfg.sshPass.length(); }

static void say(int st, const String& m) {
  xSemaphoreTake(dataLock, portMAX_DELAY);
  sshState = st; sshMsg = m;
  xSemaphoreGive(dataLock);
  Serial.printf("[ssh] %s\n", m.c_str());
  uiRequestRedraw();
}

static void sshTask(void*) {
  if (!libsshReady) { libssh_begin(); libsshReady = true; }
  ssh_session s = ssh_new();
  ssh_channel ch = nullptr;
  ssh_key srv = nullptr;
  unsigned char* hash = nullptr; size_t hlen = 0;
  char* fp = nullptr;
  int port = cfg.sshPort > 0 ? cfg.sshPort : 22;
  long timeout = 12;
  ssh_options_set(s, SSH_OPTIONS_HOST, cfg.sshHost.c_str());
  ssh_options_set(s, SSH_OPTIONS_PORT, &port);
  ssh_options_set(s, SSH_OPTIONS_USER, cfg.sshUser.c_str());
  ssh_options_set(s, SSH_OPTIONS_TIMEOUT, &timeout);
  say(1, "conectando em " + cfg.sshHost + "…");
  if (ssh_connect(s) != SSH_OK) { say(3, String("não conectou: ") + ssh_get_error(s)); goto done; }

  // the server's identity: pinned on the first use
  if (ssh_get_server_publickey(s, &srv) != SSH_OK || ssh_get_publickey_hash(srv, SSH_PUBLICKEY_HASH_SHA256, &hash, &hlen) != 0) {
    say(3, "não consegui ler a chave do servidor"); goto done;
  }
  fp = ssh_get_fingerprint_hash(SSH_PUBLICKEY_HASH_SHA256, hash, hlen);
  if (!cfg.sshFp.length()) {
    cfg.sshFp = fp; configSave();
    notifyAdd(N_AGENTE, "Servidor SSH novo guardado", cfg.sshHost + " · " + String(fp).substring(0, 24) + "…");
  } else if (cfg.sshFp != fp) {
    say(3, String("a chave do servidor MUDOU (") + String(fp).substring(7, 23) + "…). Se foi você que trocou, esqueça a antiga no portal.");
    goto done;
  }

  // login by password. Key login would add ~125 KB of code (key parsers) and does not fit next to
  // two firmware slots in 4 MB of flash; use a user made just for this, with little power.
  if (ssh_userauth_password(s, nullptr, cfg.sshPass.c_str()) != SSH_AUTH_SUCCESS) { say(3, "login recusado: confira usuário e senha"); goto done; }
  ch = ssh_channel_new(s);
  if (!ch || ssh_channel_open_session(ch) != SSH_OK || ssh_channel_request_pty_size(ch, "xterm", VT_COLS, VT_ROWS) != SSH_OK ||
      ssh_channel_request_shell(ch) != SSH_OK) { say(3, String("o servidor recusou o terminal: ") + ssh_get_error(s)); goto done; }
  xSemaphoreTake(dataLock, portMAX_DELAY); vt.reset(); xSemaphoreGive(dataLock);
  say(2, cfg.sshUser + "@" + cfg.sshHost);

  {
    static char buf[512];
    uint32_t idle = millis();
    while (!wantClose) {
      int n = ssh_channel_read_nonblocking(ch, buf, sizeof(buf), 0);
      if (n == SSH_ERROR) { say(3, "conexão caiu"); break; }
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
      if (ssh_channel_is_eof(ch) || ssh_channel_is_closed(ch)) { say(3, "sessão encerrada pelo servidor"); break; }
      String send;
      portENTER_CRITICAL(&outMux); send = outBuf; outBuf = ""; portEXIT_CRITICAL(&outMux);
      if (send.length()) { ssh_channel_write(ch, send.c_str(), send.length()); idle = millis(); }
      vTaskDelay(pdMS_TO_TICKS(n > 0 ? 2 : (millis() - idle < 2000 ? 15 : 40)));
    }
    if (wantClose) say(3, "desconectado");
  }

done:
  if (ch) { ssh_channel_close(ch); ssh_channel_free(ch); }
  if (fp) ssh_string_free_char(fp);
  if (hash) ssh_clean_pubkey_hash(&hash);
  if (srv) ssh_key_free(srv);
  ssh_disconnect(s); ssh_free(s);
  wantClose = false;
  portENTER_CRITICAL(&outMux); outBuf = ""; portEXIT_CRITICAL(&outMux);
  if (sshState != 3) sshState = 3;
  uiRequestRedraw();
  vTaskDelete(nullptr);
}

void sshStart() {
  if (sshState == 1 || sshState == 2) return;
  if (!wifiUp) { say(3, "sem Wi-Fi"); return; }
  if (!sshConfigured()) { say(3, "configure o servidor no portal (Terminal SSH)"); return; }
  // the task stack must fit in one free block; libssh works from the heap besides
  if (ESP.getMaxAllocHeap() < 40000) { say(3, "memória livre insuficiente agora; tente de novo em instantes"); return; }
  wantClose = false;
  sshState = 1;
  if (xTaskCreatePinnedToCore(sshTask, "ssh", 32768, nullptr, 1, nullptr, 0) != pdPASS) say(3, "não consegui abrir a tarefa do terminal");
}
void sshStop() { if (sshState == 1 || sshState == 2) wantClose = true; }
