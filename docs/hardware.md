# Hardware do cyberdeck

Base testada no projeto: **Orange Pi Zero 2W (4 GB)** + **CYD ESP32-2432S028** como tela de status.

## Orange Pi Zero 2W (o que importa)
- Allwinner H618, até 4 GB LPDDR4, microSD, Wi-Fi 5 + Bluetooth 5.0 com antena externa.
- Mini HDMI (sem conector DSI de tela), 2× USB-C (um é a alimentação, 5 V/2 A), 40 pinos no padrão Raspberry Pi.
- Placa de expansão opcional (~US$ 5) pelo conector de 24 pinos: 2 USB-A, P2 de áudio, Ethernet, botões.
- Kernel principal tem a placa desde o 6.8, mas a expansão ainda não; use a imagem da Orange Pi ou Armbian.

Fontes: [CNX Software](https://www.cnx-software.com/2023/09/09/orange-pi-zero-2w-raspberry-pi-zero-2w-alternative-4gb-ram/),
[linux-sunxi](https://linux-sunxi.org/Xunlong_Orange_Pi_Zero2W).

## Peças
| Função | Opção | Notas |
|---|---|---|
| Tela principal | HDMI 4–5" com toque USB | a CYD fica como tela de status sempre ligada |
| Teclado | teclado BlackBerry Q20 (I2C/USB) ou mini teclado Bluetooth | |
| Energia | 2× 18650 + placa UPS 5 V/3 A com carga USB-C passante | leitura de bateria: driver sysfs da placa ou INA219 no I2C (`deckd.ini`) |
| LoRa | nó Meshtastic no USB (mais simples) | módulo SPI SX1262 com `meshtasticd` também existe, com relatos irregulares |
| 4G / GPS | Quectel EC25-AU (mini PCIe) + adaptador USB com slot de chip | tem B28 (700 MHz), importante no interior do Brasil |
| Hub | placa de expansão ou hub USB 2.0 pequeno | CYD + Meshtastic + modem ocupam 3 portas |
| Antenas | pigtails U.FL → SMA | Wi-Fi/BT, LTE (2), GPS, LoRa |

### 4G no Brasil
- A TIM usa as bandas 28, 5, 3, 1 e 7; a de 700 MHz (B28) alcança muito mais longe
  ([Tecnoblog](https://tecnoblog.net/318966/tim-2-mil-cidades-4g-frequencia-700-mhz/)).
- SIM7600SA-H (versão "América Latina") **não tem B28** ([Waveshare](https://www.waveshare.com/product/sim7600sa-h-4g-hat.htm)).
  EC25-AU tem B1/2/3/4/5/7/8/28 e voz ([everythingRF](https://www.everythingrf.com/products/cellular-modules/quectel/811-439-ec25-au-mini-pcie)).
- Chamadas por 2G/3G funcionam nesses módulos; só VoLTE depende do firmware e da operadora
  ([ModemManager #378](https://gitlab.freedesktop.org/mobile-broadband/ModemManager/-/issues/378)).
- Confira a homologação Anatel do módulo antes de comprar.

### Ligações
- CYD: cabo USB no deck. O `deckd` acha a CYD pelo chip CH340 (1a86:7523) e abre a porta com DTR/RTS desligados
  para não reiniciar a placa.
- Meshtastic: use o caminho `/dev/serial/by-id/...` em `[mesh] port`.
- Modem: ModemManager (`apt install modemmanager`), o `deckd` lê operadora e sinal por `mmcli -J`.
- GPS: `gpsd` apontando para a porta NMEA do modem.
