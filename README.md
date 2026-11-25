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

## Gravar a CYD
```
pio run -t upload          # ou gere o .bin com `pio run` e grave em 0x10000
```
Primeiro uso: calibrar o toque (4 alvos) → escolher a rede em **Config › Wi-Fi** (procura as redes, teclado na tela, guarda até 3;
na quarta pergunta qual substituir). O resto (feed, Pi-hole, Home Assistant) se preenche pelo celular: rede **CyberCYDeck** ou o IP da placa. Tudo fica salvo na placa. Atualizações depois: `http://<ip-da-placa>/update`.

Sem Wi-Fi configurado a placa funciona igual como tela do deck.

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
`status`, `cfg`, `set <chave> <valor>` (nome, feed, token, n8n, phurl, phpass, city, mods, night, beep, theme, anim, apagao),
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

## Créditos
- Fontes Barlow, Chakra Petch e IBM Plex Mono (SIL Open Font License 1.1), convertidas para VLW por `tools_fontgen.py`
  só com os caracteres usados.
- Propagação: dados de Paul Herrman, N0NBH ([hamqsl.com](https://www.hamqsl.com/solar.html)). Órbita da ISS: [CelesTrak](https://celestrak.org/).
  Cálculo de passagens: [Sgp4-Library](https://github.com/Hopperpop/Sgp4-Library) (MIT).
- [LovyanGFX](https://github.com/lovyan03/LovyanGFX), [ArduinoJson](https://arduinojson.org/),
  protocolo do [Hermes Gadget SDK](https://github.com/Adolanium/hermes-gadget-sdk), [Meshtastic](https://meshtastic.org/).

## Licença
[MIT](LICENSE). As fontes seguem a SIL Open Font License 1.1 e as bibliotecas, as licenças delas.
