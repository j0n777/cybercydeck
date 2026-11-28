// g++ -Wall -o /tmp/rss-test tests/rss/teste.cpp src/rss.cpp && /tmp/rss-test
#include "../../src/rss.h"
#include <stdio.h>
#include <string.h>
#include <initializer_list>
static int fails = 0;
static void eq(const char* got, const char* want) {
  if (strcmp(got, want)) { printf("FALHOU: '%s' != '%s'\n", got, want); fails++; } else printf("ok   %s\n", got);
}
static void run(const char* xml, int chunk, RssReader& r) {
  r.begin(5);
  int L = strlen(xml);
  for (int i = 0; i < L; i += chunk) r.feed(xml + i, (L - i < chunk) ? L - i : chunk);
}
int main() {
  const char* rss =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?><rss version=\"2.0\" xmlns:media=\"http://search.yahoo.com/mrss/\"><channel>"
    "<title>G1 &gt; Tecnologia</title><link>x</link><image><title>ignorar</title></image>"
    "<item><title><![CDATA[Bitcoin passa de R$ 500 mil]]></title><media:title>nao</media:title><description>&lt;b&gt;x</description></item>"
    "<item><title>Pol&#237;cia &amp; ci&#xEA;ncia: &#8220;teste&#8221;</title></item>"
    "<item>\n<title>\n  Linha   quebrada\n  aqui </title></item>"
    "<item><title>Emoji 🚀 fica</title></item>"
    "</channel></rss>";
  RssReader r;
  for (int ch : {1, 3, 7, 4096}) {
    run(rss, ch, r);
    if (r.n != 4) { printf("FALHOU: n=%d (chunk %d)\n", r.n, ch); fails++; continue; }
    eq(r.source, "G1 > Tecnologia");
    eq(r.title[0], "Bitcoin passa de R$ 500 mil");
    eq(r.title[1], "Polícia & ciência: \"teste\"");
    eq(r.title[2], "Linha quebrada aqui");
    eq(r.title[3], "Emoji fica");
  }
  const char* atom =
    "<feed xmlns=\"http://www.w3.org/2005/Atom\"><title type=\"text\">Blog Atom</title>"
    "<entry><title type=\"html\">Primeiro post</title></entry><entry><title>Segundo</title></entry>"
    "<entry><title>3</title></entry><entry><title>4</title></entry><entry><title>5</title></entry><entry><title>6</title></entry></feed>";
  run(atom, 5, r);
  eq(r.source, "Blog Atom");
  eq(r.title[0], "Primeiro post"); eq(r.title[1], "Segundo");
  if (r.n != 5 || !r.done()) { printf("FALHOU: atom n=%d\n", r.n); fails++; }
  // a very long title is cut without breaking a UTF-8 char
  char big[600] = "<rss><channel><title>t</title><item><title>";
  for (int i = 0; i < 100; i++) strcat(big, "ção");
  strcat(big, "</title></item></channel></rss>");
  run(big, 11, r);
  int l = strlen(r.title[0]);
  if (l >= RssReader::TLEN || (unsigned char)r.title[0][l - 1] >= 0xC0) { printf("FALHOU: corte UTF-8 (%d)\n", l); fails++; } else printf("ok   corte em %d bytes\n", l);
  // ISO-8859-1 feed (Folha uses it): bytes become UTF-8
  const char* lat = "<?xml version=\"1.0\" encoding=\"ISO-8859-1\" ?><rss><channel><title>Folha</title><item><title>Uni\xE3" "o e a\xE7\xF5" "es</title></item></channel></rss>";
  run(lat, 9, r);
  eq(r.title[0], "União e ações");
  printf(fails ? "\n%d falhas\n" : "\ntudo certo\n", fails);
  return fails ? 1 : 0;
}
