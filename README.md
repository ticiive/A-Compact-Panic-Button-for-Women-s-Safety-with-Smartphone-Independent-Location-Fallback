# A-Compact-Panic-Button-for-Women-s-Safety-with-Smartphone-Independent-Location-Fallback

## Protótipo

### Ligações na protoboard

| Componente | Pino da placa | Observação |
|---|---|---|
| Botão | GPIO 33 e GND | Pernas diagonais do botão; sem resistor externo (usa pull-up interno) |
| Buzzer | GPIO 32 e GND | Ativo em nível alto |
| LED azul (caminho celular) | GPIO 13 e GND | Resistor de 220 a 330 Ohm em série para o GND |
| LED vermelho (GPS simulado) | GPIO 27 e GND | Resistor de 220 a 330 Ohm em série para o GND |
| LED vermelho (SMS simulado) | GPIO 26 e GND | Resistor de 220 a 330 Ohm em série para o GND |

A trilha de GND da protoboard deve ser conectada a qualquer pino GND da placa.

### Como gravar o firmware

1. Abra o Arduino IDE e instale o pacote de placas ESP32 (Espressif).
2. Em Ferramentas, selecione a placa "ESP32 Dev Module".
3. Configure Upload Speed para 115200.
4. Abra `firmware/panic_button/panic_button.ino` e clique em "Carregar".
5. Abra o Serial Monitor em 115200 baud para ver as mensagens de log.

### USE_DEEP_SLEEP

No topo do firmware ha a diretiva `#define USE_DEEP_SLEEP 0`.

- `0` (padrao): a placa fica ligada e com BLE ativo o tempo todo. Aperte o botao para iniciar um ciclo de alerta.
- `1`: a placa entra em deep sleep apos cada ciclo e acorda somente quando o botao e pressionado (pino GPIO 33 em nivel baixo). Consome muito menos bateria, mas o celular precisa reconectar a cada acionamento.

### Formato da linha RESULT no Serial Monitor

```
RESULT,tentativa,modo,caminho,tempo_ms,lat;lon;acc
```

- `tentativa`: numero sequencial do acionamento (1, 2, 3...).
- `modo`: `acordada` ou `deepsleep`, conforme USE_DEEP_SLEEP.
- `caminho`: `celular` se o ACK chegou dentro de T = 15 s, ou `fallback` se o tempo esgotou.
- `tempo_ms`: tempo em milissegundos do acionamento ate o ACK (ou `-` no fallback).
- `lat;lon;acc`: coordenadas e precisao enviadas pelo celular (ou `-` no fallback).

Exemplos:
```
RESULT,1,acordada,celular,3241,−23.550520;−46.633308;18
RESULT,2,acordada,fallback,-,-
```

### Pagina do celular

Acesse pelo navegador do smartphone (Chrome no Android, requer HTTPS para Web Bluetooth):

**https://ticiive.github.io/A-Compact-Panic-Button-for-Women-s-Safety-with-Smartphone-Independent-Location-Fallback/**

Na pagina, informe o token do bot do Telegram e o chat_id do contato de emergencia. Esses dados sao salvos apenas no navegador do celular (localStorage) e nunca enviados a nenhum servidor do projeto.
