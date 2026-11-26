# CyberCYDeck

Firmware para a **ESP32-2432S028 ("Cheap Yellow Display", CYD)** que funciona de dois jeitos:

1. **Painel de mesa (Wi-Fi):** relógio, Bitcoin e cripto, clima (pela cidade ou pelo IP), seguidores, agenda,
   manchetes, máquinas, Pi-hole, Home Assistant, agente (Hermes Gadget), rádio (propagação HF e ISS), jogos.
   Você escolhe até 4 itens da tela inicial em **Config › Início**.
2. **Tela de um cyberdeck (USB):** ligada num Orange Pi / Raspberry Pi, mostra bateria, CPU/RAM/temperatura,
   Wi-Fi, 4G, GPS e mensagens Meshtastic. Funciona **sem internet**: o relógio vem do próprio deck.

```
 ┌──────────── cyberdeck ─────────────┐
 │ Orange Pi Zero 2W (Linux)          │   USB serial: "deck {json}" a cada 3 s
 │  deckd ── bateria, 4G, GPS, mesh ──┼──────────────►  CYD (CyberCYDeck)
 │  Meshtastic no USB · modem 4G      │                 tela de status + alertas no LED
 └────────────────────────────────────┘
```

> **Estado:** a 0.2.0 roda e foi testada numa CYD real. A 0.3.0 (aprovações do agente, avisos, rádio, modo apagão)
> compila e está em teste na placa.

## Estrutura
| Pasta | O quê |
|---|---|
| `src/` | firmware PlatformIO da CYD |
| `host/` | `deckd.py` (serviço do deck), `deckd.ini.example`, `deckd.service`, `install.sh` |
| `docs/deck-protocol.md` | formato da linha `deck {json}` |
| `docs/feed.md` | JSON do feed (agenda, manchetes, máquinas, uso de IA) e dos seguidores |
| `docs/hardware.md` | lista de peças do cyberdeck e observações |
| `n8n/` | workflow do feed (máquinas, uso de IA) para importar no n8n |
| `site/` | página de gravação pelo navegador (publicada no GitHub Pages) |
| `tests/` | testes do leitor de iCal e do terminal; rodam no computador e no GitHub Actions |

## Gravar a CYD
**Pelo navegador (mais fácil):** abra **https://j0n777.github.io/cybercydeck/** no Chrome ou no Edge de um computador,
ligue a placa no USB e clique em instalar. A página sempre traz a versão mais nova. Para atualizar uma placa que já usa o
CyberCYDeck, deixe desmarcado "Erase device": Wi-Fi e configurações continuam.

**Pelo Wi-Fi:** `http://<ip-da-placa>/update`, com o arquivo `cybercydeck-<versão>-ota.bin` da
[última release](https://github.com/j0n777/cybercydeck/releases/latest) e o PIN do portal (vai num cabeçalho, nunca no endereço).

**Pelo PlatformIO:**
```
pio run -t upload          # ou grave o firmware.bin em 0x10000 com o esptool
```
A imagem `-full.bin` das releases vai em 0x0 e apaga as configurações salvas.

### Publicar uma versão
Mude `FW_VERSION` em `src/app.h` e crie a tag (`git tag v0.4.0 && git push origin v0.4.0`). O GitHub Actions compila,
anexa os `.bin` na release e atualiza a página de gravação.
Primeiro uso: calibrar o toque (4 alvos) → escolher a rede em **Config › Wi-Fi** (procura as redes, teclado na tela, guarda até 3;
na quarta pergunta qual substituir). O resto (feed, Pi-hole, Home Assistant) se preenche pelo celular: rede **CyberCYDeck** ou o IP da placa. Tudo fica salvo na placa. Atualizações depois: `http://<ip-da-placa>/update`.

Sem Wi-Fi configurado a placa funciona igual como tela do deck.

## Agenda
No Google Agenda, abra **Configurações** › o seu calendário › **Integrar agenda** e copie o
**Endereço secreto no formato iCal**. Cole no portal da placa, em **Agenda**. A placa lê o arquivo a cada 15 min,
enquanto baixa, e guarda só os próximos compromissos (eventos repetidos, cancelados e remarcados incluídos).
Serve qualquer link `.ics` (Outlook, Nextcloud, Proton...). Esse endereço dá acesso à agenda: não compartilhe; se vazar,
use **Redefinir** no Google. Sem link, a agenda vem do feed.

## Máquinas e uso de IA (n8n)
Veja [`n8n/README.md`](n8n/README.md): um workflow pronto para importar, mais o `host/sinal.py` que cada máquina roda.

## Terminal SSH
Em **Mais › Terminal**: um terminal de verdade (53×18, cores, nano, top, less, vim) num servidor seu, com teclado na tela
(esc, tab, ctrl, setas). Configure servidor, porta, usuário e senha no portal, em **Terminal SSH**.
- Login só por senha: o login por chave precisaria de ~125 KB a mais e não cabe junto com as duas cópias do firmware
  que mantêm a atualização pelo Wi-Fi. Use no servidor um usuário criado só pra isso, com pouco poder.
- A chave do servidor fica guardada na primeira conexão; se ela mudar, a placa recusa (pode ser um ataque) até você
  mandar esquecer no portal.
- Enquanto o terminal está aberto, a placa pausa as buscas de dados pela internet para sobrar memória.

## Jogos
Snake e **Tanques**: artilharia por turno contra a placa. Toque no céu para mirar (direção e força) ou ajuste
ângulo e força nos botões; o vento muda a cada rodada e a mira da placa melhora com o tempo.

## Aprovações do agente e avisos
Com o [Hermes Gadget](https://github.com/Adolanium/hermes-gadget-sdk) pareado, as perguntas do Hermes (comando perigoso,
`/new`, troca de modelo) chegam como **aprovação pendente**: a barra de cima da tela inicial avisa, o LED fica azul
e a pergunta espera na tela **Agente**. **Aprovar** pede um segundo toque em até 4 s; **Recusar** responde na hora.
Se a pergunta expirar, o Hermes trata como sem resposta. Quedas de internet, máquinas que caem ou voltam, bateria
baixa do deck e cartões do agente vão para **Avisos** (toque na barra de cima).

## Modo apagão
A placa testa a saída para a internet a cada minuto (conexão TCP a 1.1.1.1 e 9.9.9.9, sem enviar dados).
Depois de 3 min sem internet, uma tela passiva troca sozinha para **Modo apagão**: há quanto tempo caiu,
os últimos valores conhecidos com a idade de cada um, Wi-Fi e Pi-hole da rede local, ou bateria, mesh,
GPS e 4G quando o deck está ligado. Quando a internet volta, ela retorna para o início. Dá para desligar no portal.

## Rádio
Índices solares (SFI, K, A, manchas) e condição das bandas de HF de dia e de noite, do
[N0NBH](https://www.hamqsl.com/solar.html), e a próxima passagem da ISS acima de 10°, calculada na própria placa
(SGP4) com a órbita da [CelesTrak](https://celestrak.org/) e a localização da cidade do clima (ou do IP).
Os dados só são buscados quando a tela é aberta, no máximo a cada 30 min; a órbita, a cada 12 h.

## Instalar no deck (Linux)
```
cd host && sudo ./install.sh
sudo nano /etc/deckd.ini      # porta do Meshtastic, tipo de bateria...
journalctl -u deckd -f
```
Teste sem a placa: `python3 host/deckd.py --stdout --once`.

## Comandos pela USB (115200)
`status`, `cfg`, `set <chave> <valor>` (nome, feed, token, ical, n8n, phurl, phpass, city, mods, night, beep, theme, anim, apagao),
`nav <tela>`, `shot`, `tap x y`, `led <cor>`, `beep`, `restart`, `factory`, `deck {json}` (usado pelo deckd),
e para testes `hermes {json}` (injeta uma mensagem do Hermes, ex.: um `prompt`) e `notify <texto>`.

## Privacidade
Nenhuma senha, token ou endereço vem no código: tudo é digitado na própria placa e fica na memória dela.
Campos de senha nunca são mostrados de volta. O feed usa `Authorization: Bearer` com o token que você escolher.

## Roteiro
- [x] Tela Deck e Mesh alimentadas pelo host; hora vinda do deck
- [ ] Testar no Orange Pi Zero 2W com bateria, Meshtastic e modem 4G reais
- [ ] Responder mensagens Meshtastic pela tela (respostas rápidas)
- [ ] SMS e chamadas pelo modem (ModemManager)
- [ ] Case impresso em 3D
- [ ] Contador de vendas com alerta (webhook da plataforma → n8n → feed)
- [ ] Aprovações vindas do n8n, além do Hermes
- [ ] Login SSH por chave (precisa de mais espaço de firmware)
- [ ] Teclado do Cardputer para o terminal

## Créditos
- Fontes Barlow, Chakra Petch e IBM Plex Mono (SIL Open Font License 1.1), convertidas para VLW por `tools_fontgen.py`
  só com os caracteres usados.
- Propagação: dados de Paul Herrman, N0NBH ([hamqsl.com](https://www.hamqsl.com/solar.html)). Órbita da ISS: [CelesTrak](https://celestrak.org/).
  Cálculo de passagens: [Sgp4-Library](https://github.com/Hopperpop/Sgp4-Library) (MIT).
- Terminal: [LibSSH-ESP32](https://github.com/ewpa/LibSSH-ESP32), port do [libssh](https://www.libssh.org/) (LGPL-2.1).
- [LovyanGFX](https://github.com/lovyan03/LovyanGFX), [ArduinoJson](https://arduinojson.org/),
  protocolo do [Hermes Gadget SDK](https://github.com/Adolanium/hermes-gadget-sdk), [Meshtastic](https://meshtastic.org/).

## Licença
[MIT](LICENSE). As fontes seguem a SIL Open Font License 1.1 e as bibliotecas, as licenças delas.
