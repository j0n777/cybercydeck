# Feed pelo n8n (opcional)

O feed mostra as **máquinas** (CPU, RAM, temperatura, disco, online) e o **uso de IA** na placa.
A agenda não precisa dele: cole o link iCal da sua agenda no portal da placa.

## O que vem aqui
| Arquivo | O quê |
|---|---|
| `cybercydeck-feed.workflow.json` | workflow para importar no n8n: recebe os sinais das máquinas e entrega o feed |
| `montar-feed.js` | o código do nó "Montar feed" (o mesmo que está dentro do workflow) |
| `../host/sinal.py` | script que cada máquina roda para mandar o próprio estado |

Nenhum token, endereço ou ID vem nos arquivos: você coloca os seus.

## Montar
1. No n8n, crie uma **Data table** chamada `cybercydeck_cache` com as colunas `chave` (texto), `dados` (texto) e `recebido_em` (data).
2. Importe `cybercydeck-feed.workflow.json` (Workflows › Import from file).
3. Nos dois nós de cache ("Guardar no cache" e "Ler o cache"), escolha a tabela `cybercydeck_cache`.
4. Nos nós "Token confere?" e "Bearer confere?", troque `TROQUE-PELO-SEU-TOKEN` por um token longo seu
   (ex.: `openssl rand -hex 24`). O mesmo token nos dois.
5. Ative o workflow. Ficam dois endereços:
   - `https://SEU-N8N/webhook/cybercydeck-sinal`: as máquinas mandam o estado (`POST`, cabeçalho `x-cyd-token`).
   - `https://SEU-N8N/webhook/cybercydeck-feed`: a placa lê (`GET`, `Authorization: Bearer`).
6. Em cada máquina: `pip install psutil` e
   `CYD_SINAL_URL=https://SEU-N8N/webhook/cybercydeck-sinal CYD_TOKEN=seu-token python3 host/sinal.py`
   (ou `--uma-vez` num cron de 1 min). Teste antes com `python3 host/sinal.py --mostrar`.
7. No portal da placa: **Feed** = o endereço do feed e **Token do feed** = o mesmo token.

Para mostrar nomes bonitos e uma ordem fixa, edite `NOMES` e `ORDEM` no nó "Montar feed".
Uma máquina some se ficar 10 min sem mandar (vira offline) e sai da lista depois de 7 dias.

## Uso de IA (opcional)
O feed soma chaves `uso_cc_<máquina>` no mesmo cache, no formato diário do
[ccusage](https://github.com/ryoppippi/ccusage) (`ccusage daily --json`): mande esse JSON por `POST` para o
endereço de sinal com `?chave=uso_cc_<máquina>`.
