# Dados dos experimentos

## Como os dados foram coletados

### Equipamento

- Placa: ESP32-WROVER-DEV, firmware compilado com `USE_DEEP_SLEEP 1` e `MEASURE_MODE 1` (T = 60 s para permitir medir a reconexão completa sem acionar o fallback).
- O papel do celular foi feito pela página `docs/index.html` aberta no Chrome de um MacBook (Web Bluetooth + API de geolocalização).
- Localização obtida por Wi-Fi do notebook, com precisão informada de aproximadamente 35 m. Na maioria das tentativas o navegador reutilizou a posição em cache do sistema operacional (tempo de dezenas de milissegundos), de modo que os tempos de ACK são um limite inferior para um celular que precise obter posição GNSS nova.
- Envio ao contato de emergência via Telegram Bot API.

### Captura das linhas MEAS pelo Terminal

O processo `serial-monitor` da Arduino IDE ocupa a porta serial e impede leituras paralelas. Para coletar os dados sem interferência, a IDE foi fechada e o Terminal foi usado diretamente:

```bash
(stty 115200; cat) < /dev/cu.usbserial-110 | grep --line-buffered -E "MEAS|fallback" | tee -a ~/Desktop/meas.txt
```

O filtro `grep -E "MEAS|fallback"` mantém apenas as linhas de instrumentação e de fallback, descartando as linhas RESULT que contêm as coordenadas reais.

### Formato das linhas capturadas

```
MEAS,tentativa,conectou_ms,ack_ms
```

- `tentativa`: número sequencial contado na RTC RAM (sobrevive ao deep sleep).
- `conectou_ms`: tempo em ms desde o wake-up até a primeira conexão BLE.
- `ack_ms`: tempo em ms desde o wake-up até o ACK escrito pelo celular.

Ambos os tempos são contados a partir de `millis()` no início do `setup()`, que recomeça a cada wake-up. O tempo de boot da ROM anterior à aplicação não está incluído.

## Arquivos

| Arquivo | Conteúdo |
|---|---|
| `deepsleep_meas.csv` | Medições individuais com a placa em deep sleep (22 tentativas) |
| `awake_summary.csv` | Estatística resumida da série com a placa acordada (16 tentativas) |
| `analyze.py` | Script Python 3 que lê `deepsleep_meas.csv` e imprime as estatísticas |

## Como rodar o script

```bash
python3 results/analyze.py
```

Não requer dependências externas além da biblioteca padrão do Python 3.
