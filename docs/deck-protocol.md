# Protocolo deck → CYD (v1)

O host escreve uma linha por atualização na serial USB da CYD (115200 8N1), terminada em `\n`:

```
deck {"v":1,"t":1791900000,"host":"zero2w","cpu":12,"ram":34,"temp":48,"up":3600,
      "bat":{"pct":82,"v":7.92,"chg":true},
      "wifi":{"ssid":"Casa","ip":"192.168.0.40","rssi":-55},
      "lte":{"op":"TIM","sig":60},
      "gps":{"fix":true,"sats":7},
      "mesh":{"nodes":5,"msgs":[{"de":"BASE","txt":"chegando","t":1791899950}]},
      "alert":"bateria baixa: 12%"}
```
(numa linha só; quebrada aqui para leitura)

| Campo | Tipo | Observação |
|---|---|---|
| `v` | int | versão do protocolo (1) |
| `t` | epoch s | se o relógio da CYD não estiver certo (sem Wi-Fi), ela acerta por este |
| `host` | texto | nome mostrado na tela |
| `cpu`, `ram` | % | -1 = desconhecido |
| `temp` | °C | maior zona térmica |
| `up` | s | tempo ligado |
| `bat` | objeto | `pct` 0–100, `v` volts, `chg` carregando. Sem `bat` = sem leitura de bateria |
| `wifi` | objeto | `ssid`, `ip`, `rssi` (dBm) |
| `lte` | objeto | `op` operadora, `sig` 0–100 |
| `gps` | objeto | `fix`, `sats` |
| `mesh` | objeto | `nodes` conhecidos, `msgs` até 3, mais nova primeiro (`de`, `txt`, `t`) |
| `alert` | texto | aparece em vermelho no rodapé da tela Deck |

Regras:
- Linha com no máximo 1500 bytes (o `deckd` corta mensagens se passar).
- Qualquer campo pode faltar. A CYD considera o deck desconectado após 15 s sem linha.
- Bateria ≤ 15% sem carregar acende o LED vermelho e mostra um aviso.
- Ao abrir a porta, mantenha **DTR e RTS desligados**: na CYD eles ligam no reset e no modo de gravação.
- A CYD não responde nada; o que sai dela é só o log normal, que o host pode descartar.
