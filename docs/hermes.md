# Agente: parear com o Hermes

A tela **Agente** conversa com o Hermes Agent pelo protocolo do
[Hermes Gadget SDK](https://github.com/Adolanium/hermes-gadget-sdk) (WebSocket). A placa mostra o estado, o código de
pareamento, cartões e as aprovações pendentes (comando perigoso, `/new`, troca de modelo).

Fonte dos passos: `README.md` e `docs/hermes-integration.md` do Hermes Gadget SDK.

## 1. No computador do Hermes
```
hermes-gadget plugin install
hermes plugins enable gadget
hermes config set platforms.gadget.enabled true
```
Em `~/.hermes/config.yaml`, o adaptador precisa escutar na rede local (não só em 127.0.0.1):
```yaml
platforms:
  gadget:
    enabled: true
    extra:
      host: 0.0.0.0        # interface onde os aparelhos conectam
      port: 8765
      path: /gadget
      unauthorized_dm_behavior: pair
```
Opcional, em `~/.hermes/.env`: `GADGET_ACCESS_TOKEN=<um token seu>` (todo aparelho precisa apresentar esse token).

Reinicie o gateway (`hermes gateway run`, ou o serviço que você usa). **Isso derruba as conversas abertas nos outros
canais por alguns segundos.** Libere a porta 8765 no firewall só para a rede local.

## 2. Na placa
1. Ligue o módulo **Agente** em Config › Módulos.
2. No portal (Config › Pelo celular), em **Agente · Hermes Gadget**, coloque `ws://<ip-do-hermes>:8765/gadget`
   e, se usou, o token.
3. A tela **Agente** mostra um código.

## 3. Aprovar
No computador do Hermes:
```
hermes pairing approve gadget <CÓDIGO>
```
Em até ~2 s a placa recebe `paired` e passa a mostrar o agente, sem reconectar. Para tirar o acesso:
`hermes pairing revoke gadget <id>` (o id aparece em `hermes gadget devices`).

## Problemas comuns
- **"conectando" o tempo todo:** o Hermes está escutando só em 127.0.0.1, a porta está bloqueada, ou o IP mudou.
  Teste do celular na mesma rede: `http://<ip-do-hermes>:8765/gadget` deve responder (erro de "upgrade" é bom sinal).
- **Código não aparece:** veja `http://<ip-da-placa>/log` (linhas `[hermes]`).
- `GADGET_ALLOWED_USERS` faz o Hermes ignorar aparelhos desconhecidos em vez de pedir pareamento: mantenha
  `unauthorized_dm_behavior: pair`.
