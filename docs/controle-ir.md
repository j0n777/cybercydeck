# Controle IR (em espera)

A tela **Controle IR** existe só como desenho: **o firmware ainda não emite infravermelho**. O módulo vem desligado.
Fica em espera porque:

- a CYD não tem LED IR: precisa montar um pequeno circuito no conector **CN1** (GPIO27, 3,3 V, GND);
- o firmware está em ~96% do espaço da partição (as duas cópias que permitem atualizar pelo Wi-Fi), e uma biblioteca
  de IR com os protocolos comuns de TV e ar-condicionado ocupa bastante.

Não tem relação com o Orange Pi: é um recurso da própria CYD.

## Se quiser montar o hardware desde já
| Peça | Observação |
|---|---|
| LED IR 940 nm (5 mm) | o comum de controle remoto |
| Transistor NPN (2N2222 ou BC337) | o GPIO sozinho não dá corrente suficiente para alcance bom |
| Resistor 1 kΩ | GPIO27 → base do transistor |
| Resistor 33–47 Ω | 3,3 V (ou 5 V) → LED → coletor; limita a corrente do LED |
| Receptor IR 38 kHz (VS1838B), opcional | para aprender códigos do controle original |

Ligação: GPIO27 → 1 kΩ → base; emissor → GND; 5 V → resistor → anodo do LED; catodo → coletor.

Quando houver espaço (por exemplo, retirando um módulo pouco usado), a ideia é usar a
[IRremoteESP8266](https://github.com/crankyoldgit/IRremoteESP8266) só com os protocolos necessários.
