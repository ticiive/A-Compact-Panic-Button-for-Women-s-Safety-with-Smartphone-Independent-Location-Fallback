# Demonstração na bancada

Abra [o firmware integrado](../firmware/panic_button/panic_button.ino) na Arduino IDE. A montagem usa ESP32 Wrover Module e core Espressif 3.3.12. Para voz, instale a biblioteca Arduino exportada do projeto Edge Impulse `ticiive-project-2_inferencing`, versão 1.0.4. A biblioteca gerada é uma dependência externa. O perfil entregue tem `ACTIVATION_MODE 1`, `KWS_TEST_MODE 0` e aceita os dois acionamentos. Para demonstrar o despertar físico do sono profundo, compile com `ACTIVATION_MODE 0`. Carregue o sketch e feche o monitor serial.

1. Abra o [Painel de Emergência para Mac Apple Silicon](../macos/Painel_Emergencia.dmg). Ative a recepção, permita Bluetooth e localização e feche qualquer outro cliente BLE conectado à mesma placa. O primeiro pareamento tem uma janela de até 60 s após ligar a placa. Na alternativa Web Bluetooth, sirva [a página do projeto](index.html) por HTTPS ou localhost em navegador compatível.
2. Na raiz do projeto, instale as dependências e inicie a ponte. Em modo de botão sem voz, acrescente `--no-mic`. A opção `--fixed` determina só a posição do GNSS emulado. O painel fornece a localização do Mac no caminho BLE.

   ```sh
   python3 -m pip install pyserial sounddevice numpy
   python3 tools/pc_bridge.py --fixed
   ```

3. Com o painel ativo, pressione e solte o botão. Observe o início da emergência, a origem da posição e a confirmação de aceitação do SMS emulado. Aguarde o próximo envio aos 60 s e segure o botão por 3 s para encerrar.
4. Para demonstrar o fallback, pause a recepção no painel. Com a placa em escuta, diga “abacaxi”. Observe a detecção, o início da emergência, o esgotamento do prazo de 6 s e a aquisição GNSS emulada. Para ensaiar ausência de posição, use `--cold 120` e aguarde o timeout de 90 s. Se já houver posição armazenada, ela será identificada como última conhecida.
5. Para mostrar envio ao Telegram, configure `TELEGRAM_BOT_TOKEN` no ambiente e acrescente `--telegram ID_DO_CHAT` à ponte. Confira a entrega no chat. A página BLE também oferece um envio opcional próprio; habilite apenas um encaminhador para a demonstração. O painel nativo responde só pela localização.

O [gravador opcional](../demobotao.py) inicia a própria ponte e mostra a câmera numa janela. Feche a ponte anterior, instale `opencv-python` no mesmo Python e execute `python3 demobotao.py`. Com painel e câmera visíveis na tela principal, a gravação usa o recurso de captura do macOS e salva o vídeo em Filmes/DemoBotao. O Mac precisa permitir câmera e gravação de tela ao aplicativo que executa o script.

O [ensaio isolado de acionamento](../experiments/activation/LEIA-ME.txt) usa outro sketch. Após o ensaio, carregue novamente o firmware integrado para o vídeo. O código do painel e sua configuração estão em [macos](../macos/).

## Montagem

| Sinal do firmware | GPIO da ESP32 |
|---|---|
| Botão de acionamento e despertar, ligado ao GND | 33 |
| Indicador tátil/sonoro | 32 |
| LEDs de posição BLE, GNSS e modem | 13, 27 e 26 |
| Controle projetado de alimentação GNSS | 25 |
| RX para entrada GNSS | 34 |
| RX e TX para modem | 35 e 14 |

Os pinos dos módulos descrevem a integração projetada. No ensaio, `GPS_ON_PC=1` e `GSM_ON_PC=1` selecionam a serial USB para ambos. A alimentação dos módulos e o microfone embarcado ainda precisam de integração e validação.

Para recompilar o painel no próprio Mac, execute `sh macos/build.sh` na raiz do repositório. O aplicativo compilado localmente fica em `macos/build/Painel.app`.
