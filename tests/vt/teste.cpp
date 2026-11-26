// Teste do terminal (VT100) no computador: g++ -o /tmp/vt tests/vt/teste.cpp src/vt.cpp && /tmp/vt
#include "../../src/vt.h"
#include <stdio.h>
#include <string.h>

static Vt t;
static int fails = 0;
static void feed(const char* s) { t.feed(s, strlen(s)); }
static void row(int r, const char* want) {
  char got[VT_COLS + 1]; int n = 0;
  for (int c = 0; c < VT_COLS; c++) got[n++] = t.cell[r][c].ch < 128 ? (char)t.cell[r][c].ch : '?';
  while (n && got[n - 1] == ' ') n--;
  got[n] = 0;
  if (strcmp(got, want)) { printf("linha %d: esperava \"%s\", veio \"%s\"\n", r, want, got); fails++; }
}
static void check(bool ok, const char* what) { if (!ok) { printf("falhou: %s\n", what); fails++; } }

int main() {
  t.reset();
  feed("user@host:~$ ls\r\nbin  etc\r\n");
  row(0, "user@host:~$ ls"); row(1, "bin  etc"); check(t.cy == 2 && t.cx == 0, "cursor depois de CR LF");

  feed("\x1b[2J\x1b[H\x1b[1;31mERRO\x1b[0m ok");               // limpa, cor, volta ao normal
  row(0, "ERRO ok"); check(t.cell[0][0].fg == 1 && (t.cell[0][0].attr & VT_BOLD), "vermelho em negrito");
  check(t.cell[0][5].fg == 255, "cor volta ao padrão");

  feed("\x1b[5;10HX\x1b[6n");                                  // posiciona e pede a posição
  check(t.cell[4][9].ch == 'X', "ESC[5;10H"); check(!strncmp(t.reply, "\x1b[5;11R", t.replyLen), "resposta de posição");

  t.reset();                                                     // rola quando passa da última linha
  for (int i = 1; i <= VT_ROWS + 2; i++) { char b[16]; snprintf(b, sizeof b, "%d\r\n", i); feed(b); }
  row(0, "4"); row(VT_ROWS - 2, "20");

  t.reset();                                                     // quebra no fim da linha
  for (int i = 0; i < VT_COLS + 3; i++) feed("a");
  check(t.cy == 1 && t.cx == 3, "quebra automática");

  t.reset();                                                     // região de rolagem (como o nano/less)
  feed("\x1b[1;1Hcabeçalho\x1b[2;5r\x1b[5;1Hx\n\ny\x1b[r");
  row(0, "cabe?alho"); row(2, "x"); row(4, " y");   // o x subiu duas linhas dentro da região

  t.reset();                                                     // tela alternativa (vim) e desenho de linhas
  feed("antes\x1b[?1049h\x1b[H\x1b(0lqqk\x1b(B");
  row(0, "+--+"); feed("\x1b[?1049l"); row(0, "");

  t.reset();                                                     // UTF-8 e caixa
  feed("ação \xe2\x94\x80\xe2\x94\x82");
  check(t.cell[0][1].ch == 0xE7 && t.cell[0][2].ch == 0xE3, "acentos"); check(t.cell[0][5].ch == '-' && t.cell[0][6].ch == '|', "caixa");

  t.reset(); feed("\x1b[?1h"); check(t.appCursor, "setas em modo aplicação"); feed("\x1b[?25l"); check(!t.cursorOn, "cursor escondido");

  puts(fails ? "FALHOU" : "ok");
  return fails ? 1 : 0;
}
