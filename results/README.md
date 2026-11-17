# Ensaios de Bluetooth da AP1

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

## Ensaios de voz da AP2

Os quatro registros de 15/11/2026 geraram dois CSVs. Cada linha de `voice_trials.csv` identifica o horário de início (HHMMSS), condição, tentativas e detecções. O denominador da primeira rodada foi anotado no ensaio. Cada linha de `voice_detections.csv` representa um evento KWS. A latência da coluna `quiet_latency_ms` vai do fim estimado da fala até a linha de detecção na USB. Nas rodadas com vídeo, o detector de fim de fala também reage às vozes de fundo, por isso essa coluna fica vazia.

O teste funcional curto de 16/11 contém duas marcações de tentativa depois de uma detecção. Ele não integra as quatro rodadas controladas. A eficácia observada de 96% por condição descreve somente tentativas intencionais. Os CSVs não contêm áudio, coordenadas pessoais nem identificadores de chat.

Os eventos selecionados em [fallback_examples.csv](fallback_examples.csv) mostram a troca de fonte entre os envios 1 e 2 da sessão 233841 e o esgotamento da janela GNSS no envio 61 da sessão 212851 (14/11). O campo de duração segue o evento de origem. As coordenadas e o texto integral das mensagens foram omitidos.
