# Feeds HTTP (modo painel, com Wi-Fi)

Os dois são URLs que você configura na placa (Config › Pelo celular). Pode ser um JSON estático num servidor,
um webhook do n8n, um script seu. A placa só faz `GET`.

## Feed do servidor (agenda, manchetes, máquinas, uso de IA)
Lido a cada 5 min. Se tiver token, vai como `Authorization: Bearer <token>`.
Aceita o objeto direto ou embrulhado em `{"ok":true,"dados":{...}}`. `{"pendente":true}` = ainda sem dados.

```json
{
  "saude": "ok",
  "agenda": [
    {"t": "Reunião com cliente", "quando": "2026-10-13T14:00:00-03:00", "dia_todo": false}
  ],
  "manchetes": [
    {"t": "Título da notícia", "fonte": "site"}
  ],
  "maquinas": [
    {"n": "servidor", "cpu": 12, "ram": 40, "temp": 51, "disco": 62, "on": true, "idade_s": 40}
  ],
  "ia": {"custo_usd": 72.76, "tokens": 1290000000, "por_maquina": [{"n": "pc", "custo": 50.1}]},
  "gerado_em": "2026-10-12T20:00:00-03:00"
}
```
- `saude`: `ok` | `alerta` | `critico` (pinta o "S" da tela inicial; `critico` acende o LED vermelho).
- Até 3 itens de agenda, 5 manchetes, 5 máquinas, 4 em `por_maquina`. Máquina com `on:false` acende o LED vermelho.
- Máquinas: números inteiros (-1 = sem leitura). `disco` é o % do disco mais cheio; `idade_s` é há quanto tempo a máquina mandou o último sinal. As 3 primeiras aparecem na tela inicial.
- Exemplo de servidor: `n8n/montar-feed.js` monta esse JSON a partir de um cache com uma chave por máquina.
- Datas em ISO-8601; sem fuso, assume −03:00.

## Seguidores
Lido a cada 15 min, sem token.
```json
{"total": 12345, "instagram": 0, "youtube": 0, "tiktok": 0, "x": 0, "telegram": 0, "threads": 0, "facebook": 0}
```
Sem `total`, a placa soma as redes. `{"ok":false}` = sem dados.
