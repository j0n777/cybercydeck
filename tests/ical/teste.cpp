// Teste do leitor de iCal no computador: g++ -o /tmp/t tests/ical/teste.cpp src/ical.cpp && /tmp/t tests/ical/exemplo.ics
#include "../../src/ical.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(int argc, char** argv) {
  FILE* f = fopen(argc > 1 ? argv[1] : "tests/ical/exemplo.ics", "rb");
  if (!f) { puts("sem arquivo"); return 2; }
  static char b[1 << 16]; int n = fread(b, 1, sizeof b, f); fclose(f);
  IcsAgenda a;
  const long long now = 1795649400;          // 2026-11-25 20:30 em UTC-3
  a.begin(now, now + 7 * 86400, -10800);
  for (int i = 0; i < n; i += 97) a.feed(b + i, n - i < 97 ? n - i : 97);   // pedaços tortos, como na rede
  IcsEvent e[16]; int k = a.finish(e, 16);
  const char* want[] = {"Começou há 15 min", "Reunião com cliente, sala 2", "Evento em UTC (9h local)", "Aniversário da Ana",
                        "Treino (mudou para 10h)", "Aniversário do João", "Encontro mensal (1ª terça)", "Treino"};
  int nw = sizeof want / sizeof *want, fail = 0;
  if (k != nw) { printf("esperava %d eventos, veio %d\n", nw, k); fail = 1; }
  for (int i = 0; i < k && i < nw; i++)
    if (strncmp(e[i].title, want[i], strlen(want[i]))) { printf("%d: esperava \"%s\", veio \"%s\"\n", i, want[i], e[i].title); fail = 1; }
  puts(fail ? "FALHOU" : "ok");
  return fail;
}
