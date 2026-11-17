# Hardware, software e modelo de voz

## Evolução do protótipo

Na AP1, a ESP32 acordava com o botão físico, anunciava um serviço BLE e aguardava a confirmação de uma página aberta no Chrome. A página obtinha a posição e fazia o pedido ao Telegram antes de confirmar à placa. Um LED azul indicava a confirmação. Os LEDs vermelhos permitiam observar a decisão de seguir para GNSS e GSM. O prazo de projeto apresentado era de 15 s. A série de medição após sono profundo usou 60 s para capturar reconexões demoradas.

Na AP2, o grupo implementou o acionamento alternativo por voz, o envio periódico até o cancelamento, a interpretação de sentenças de posição e a emissão de comandos de SMS. O painel nativo do Mac passou a oferecer uma segunda implementação do cliente BLE. O prazo de 6 s foi escolhido a partir das medições históricas. O GNSS e o modem continuam emulados na bancada. A integração física desses módulos e a medição de corrente com bateria permanecem como trabalhos futuros.

## Hardware e software

| Parte | Montagem e software | O que executa |
|---|---|---|
| Controlador | Placa ESP32-WROVER alimentada por USB, Arduino com core Espressif 3.3.12 | Lê o botão, executa a inferência quando habilitada, decide a fonte de posição, solicita o SMS e conserva o estado da emergência na memória RTC. |
| Interface física | Protoboard, botão, LEDs e indicador sonoro da montagem AP1 | O botão acorda a placa ou aciona a emergência. Uma pressão de 3 s permite cancelar. Os indicadores dão visibilidade às etapas da bancada. |
| Localização pelo cliente | Página JavaScript com Web Bluetooth e geolocalização, ou aplicativo Swift com Core Bluetooth e Core Location | Recebe a notificação BLE e escreve latitude, longitude e precisão na característica de confirmação. |
| Áudio | Microfone do Mac, Python e transporte serial USB | Fornece áudio PCM mono à ESP32. A placa extrai as características e classifica a palavra. |
| Periféricos emulados | Ponte Python, sentenças NMEA RMC e respostas AT | Representa o receptor NEO-6M e o modem SIM800L. O encaminhamento opcional ao Telegram permite observar o texto de teste no destino. |
| Alimentação projetada | Bateria de 2.000 mAh no simulador Python | Permite estimar autonomia por perfil de escuta e intervalo de envio. A bancada usa USB. |

[Foto da montagem](figures/prototipo.jpg) · [Ligações e execução da bancada](demonstracao.md) · [Firmware](../firmware/panic_button/panic_button.ino) · [Ponte USB](../tools/pc_bridge.py) · [Painel nativo](../macos/Painel.swift)

## Modelo de inteligência computacional

O projeto usa reconhecimento de palavra-chave com três saídas: `abacaxi`, `noise` e `unknown`. O grupo selecionou a palavra em português, preparou amostras de um segundo e integrou a biblioteca Arduino exportada do Edge Impulse, `ticiive-project-2_inferencing` 1.0.4. Os parâmetros abaixo correspondem aos arquivos `model_metadata.h` e `model_variables.h` dessa exportação e à rotina de escuta do firmware.

| Etapa | Configuração da AP2 |
|---|---|
| Entrada | Áudio mono a 16 kHz. A janela de um segundo contém 16.000 amostras. A inferência contínua recebe fatias de 4.000 amostras, equivalentes a 250 ms. |
| Extração de características | Energias em bandas de frequência na escala mel (MFE), com 40 filtros, quadros de 20 ms e avanço de 10 ms. A janela completa produz 3.960 características para a rede. |
| Classificador | Rede quantizada, com entrada e saída int8, exportada como grafo compilado. A ESP32 executa o classificador durante a escuta a 160 MHz. |
| Decisão | A rotina lê a pontuação de `abacaxi`. Duas saídas consecutivas de pelo menos 0,80 disparam o mesmo procedimento de emergência usado pelo botão. Uma pontuação abaixo desse limiar zera a contagem. |

A alternativa de transfer learning foi comparada com um classificador menor. As estimativas registradas passaram de 826 para 11 ms de inferência e de 166,2 para 14,7 kB de RAM do classificador. Já os 216 ms publicados no artigo são uma medição de bancada, do fim estimado da fala até o evento de detecção na USB. Os quatro ensaios com palavras intencionais totalizaram 48/50 detecções em silêncio e 48/50 com vídeo ao fundo. A taxa de falsos alarmes por hora requer um ensaio separado.

[Preparação de amostras](../tools/split_keyword.py) · [Contagens por rodada](../results/voice_trials.csv) · [Detecções individuais](../results/voice_detections.csv) · [Ensaio dedicado](../experiments/activation/LEIA-ME.txt)

Para compilar o perfil de voz, a Arduino IDE precisa localizar o cabeçalho `ticiive-project-2_inferencing.h` da exportação 1.0.4. O código de integração e a lógica de decisão estão no firmware do repositório. A biblioteca gerada é instalada separadamente.
