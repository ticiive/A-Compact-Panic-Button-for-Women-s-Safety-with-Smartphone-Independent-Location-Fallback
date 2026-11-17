#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <sys/time.h>
#include "driver/rtc_io.h"
#include "driver/gpio.h"

// =====================================================================
//  PARÂMETROS EDITÁVEIS
// =====================================================================
const uint32_t DELTA_S   = 60;    // <<< INTERVALO ENTRE ENVIOS (Δ), em segundos. Teste: 60 (o fallback leva ~30 s). Versão final: 240 (4 min)
const uint32_t T_ACK_MS  = 6000;  // <<< TEMPO DE ESPERA PELA RESPOSTA DO CELULAR (T), em ms
const uint32_t HOLD_MS   = 3000;  // <<< TEMPO SEGURANDO O BOTÃO PARA ENCERRAR A EMERGÊNCIA, em ms
#define PRINT_COORDS 0            // <<< 0 = esconde latitude/longitude no log (seguro para compartilhar); 1 = mostra
#define ACTIVATION_MODE 1         // <<< 0 = só botão (deep sleep parado); 1 = botão OU palavra secreta (placa acordada ouvindo)
#define KEYWORD "abacaxi"         // <<< rótulo da palavra secreta, igual ao do Edge Impulse
const float    KWS_THRESHOLD = 0.80f;  // <<< confiança mínima (0 a 1) para aceitar a palavra
const int      KWS_HITS      = 2;      // <<< quantas janelas seguidas acima do limiar para disparar
#define KWS_TEST_MODE 0           // <<< 1 = SÓ TESTE DA VOZ: fica ouvindo sem parar, bipa a cada "abacaxi", sem emergência
const uint32_t LISTEN_CPU_MHZ = 160;   // <<< clock enquanto ouve: 160 MHz gasta menos que 240 e ainda dá conta (80 não)
#define GPS_ON_PC 1
#define GSM_ON_PC 1           // <<< 1 = GPS e SIM800L imitados pelo PC (tools/pc_bridge.py) pela USB; 0 = módulos reais
#define CONTACT_NUMBER "+5521900000000"  // <<< número do contato de emergência que recebe o SMS
const uint32_t GPS_TIMEOUT_S = 90;     // <<< tempo máximo esperando posição do GPS (partida a frio ~27 s)
const uint32_t PAIR_S        = 60;     // <<< ao ligar, janela para conectar a página do celular pela 1ª vez
#define USE_PHONE_PATH 1          // <<< 1 = tenta o celular primeiro e, se falhar, usa GPS + SMS do botão; 0 = só GPS + SMS
// =====================================================================

#if ACTIVATION_MODE == 1
// Biblioteca gerada pelo Edge Impulse (Deployment > Arduino library). O nome segue o nome do projeto:
// projeto "ticiive-project-2" -> ticiive-project-2_inferencing.h
#define EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW 4
#define EIDSP_QUANTIZE_FILTERBANK 0       // igual ao exemplo do Edge Impulse para ESP32
#include <ticiive-project-2_inferencing.h>
const int FRAME_SAMPLES = 500;    // amostras por pacote vindo do PC (igual ao script)
static_assert(EI_CLASSIFIER_SLICE_SIZE % FRAME_SAMPLES == 0, "SLICE_SIZE deve ser multiplo de FRAME_SAMPLES");
static int16_t slice[EI_CLASSIFIER_SLICE_SIZE];
#endif
// Serial USB rápida: carrega o log, o áudio do modo voz (32 kB/s) e, com MODULES_ON_PC, o GPS e o SIM800L imitados
const uint32_t BAUD = 460800;     // 921600 saiu ilegível no Mac; 460800 ainda dá folga para os 32 kB/s do áudio

// Pinos
const int PIN_BUTTON = 33;      // botão; a outra perna (diagonal) vai no GND
const int PIN_VIBRA  = 32;      // motor de vibração (via transistor); até ele chegar, o buzzer fica aqui
const int LED_PHONE  = 13;      // LED azul: caminho do celular (ACK recebido)
const int LED_GNSS   = 27;      // LED vermelho: GPS do botão (simulado)
const int LED_GSM    = 26;      // LED vermelho: SMS do botão (simulado)
// Pinos dos módulos reais (MODULES_ON_PC 0). Na WROVER, GPIO16/17 são da PSRAM: não usar.
const int PIN_GPS_PWR = 25;     // liga/desliga a alimentação do GPS (via MOSFET); desligado = partida a frio
const int GPS_RX = 34, GPS_TX = -1;   // a ESP32 só escuta o GPS
const int GSM_RX = 35, GSM_TX = 14;

#if GPS_ON_PC
Stream& gps = Serial;
#else
HardwareSerial gps(2);
#endif
#if GSM_ON_PC
Stream& gsm = Serial;
#else
HardwareSerial gsm(1);
#endif

// UUIDs do serviço Nordic UART (reutilizados com semântica própria ALERT/ACK)
#define SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define ACK_UUID     "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  // celular -> ESP32 (write)
#define ALERT_UUID   "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  // ESP32 -> celular (notify)

// Estado da emergência: fica na RTC RAM e sobrevive ao deep sleep
RTC_DATA_ATTR bool     emergencyActive = false;  // há uma emergência em andamento
RTC_DATA_ATTR bool     bleFailed       = false;  // o BLE já falhou nesta emergência
RTC_DATA_ATTR uint32_t sendCount       = 0;      // envios feitos nesta emergência
RTC_DATA_ATTR uint32_t emergencyId     = 0;      // contador de emergências (para o log)
RTC_DATA_ATTR uint64_t nextSendMs      = 0;      // instante do próximo envio (relógio RTC)
RTC_DATA_ATTR uint64_t startMs         = 0;      // instante do início da emergência (relógio RTC)
RTC_DATA_ATTR bool     haveLast        = false;  // há uma última posição conhecida
RTC_DATA_ATTR float    lastLat = 0, lastLon = 0; // última posição conhecida (celular ou GPS)

// Globais BLE
BLECharacteristic* alertChar = nullptr;
BLEServer* server = nullptr;
volatile bool connected = false;
volatile bool ackReceived = false;
volatile uint32_t tConnect = 0;
String ackPayload = "";

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override {
    connected = true;
    if (tConnect == 0) tConnect = millis();
  }
  void onDisconnect(BLEServer* s) override {
    connected = false;
    BLEDevice::startAdvertising();
  }
};

class AckCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    ackPayload = String(c->getValue().c_str());
    ackReceived = true;
  }
};

// ---- Utilitários ----
// Relógio em ms que continua contando durante o deep sleep (millis() zera a cada wake-up)
uint64_t nowMs() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000ULL + tv.tv_usec / 1000ULL;
}

// Segundos desde o início da emergência (para conferir o intervalo no log)
float elapsedS() {
  return (float)(nowMs() - startMs) / 1000.0f;
}

// Esconde latitude e longitude no log, mantendo só a precisão (acc)
String safePayload(const String& p) {
#if PRINT_COORDS
  return p;
#else
  int last = p.lastIndexOf(';');
  return last >= 0 ? "*;*;" + p.substring(last + 1) : "*";
#endif
}

void vibrate(int times) {
  for (int i = 0; i < times; i++) {
    digitalWrite(PIN_VIBRA, HIGH);
    delay(250);
    digitalWrite(PIN_VIBRA, LOW);
    if (i < times - 1) delay(200);
  }
}

void ledsOff() {
  digitalWrite(LED_PHONE, LOW);
  digitalWrite(LED_GNSS, LOW);
  digitalWrite(LED_GSM, LOW);
}

void setupBLE() {
  BLEDevice::init("PanicButton");
  server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  BLEService* service = server->createService(SERVICE_UUID);

  alertChar = service->createCharacteristic(ALERT_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  alertChar->addDescriptor(new BLE2902());

  BLECharacteristic* ackChar = service->createCharacteristic(
      ACK_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  ackChar->setCallbacks(new AckCallbacks());

  service->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
}

// ---- Cancelamento enquanto a placa está ocupada ----
// Antes, segurar o botão só era lido quando a placa acordava pelo botão; durante o GPS, o
// Bluetooth ou o SMS ela estava acordada e não via o aperto. Agora os laços longos chamam isto.
volatile bool cancelRequested = false;
void endEmergency();
bool pollCancel() {
  static uint32_t pressStart = 0;
  if (!emergencyActive) return false;
  if (digitalRead(PIN_BUTTON) == LOW) {
    if (pressStart == 0) { pressStart = millis(); Serial.println("BOTAO,pressionado"); }
    else if (millis() - pressStart >= HOLD_MS) cancelRequested = true;
  } else {
    pressStart = 0;
  }
  return cancelRequested;
}

// ---- Leitura de linhas e espera de respostas (GPS e SIM800L) ----
bool readLineFrom(Stream& in, char* buf, size_t n, uint32_t timeoutMs) {
  size_t i = 0;
  uint32_t t = millis();
  while (millis() - t < timeoutMs) {
    if (pollCancel()) break;
    int c = in.read();
    if (c < 0) { delay(2); continue; }
    if (c == '\n') { buf[i] = 0; return i > 0; }
    if (c != '\r' && i < n - 1) buf[i++] = (char)c;
  }
  buf[i] = 0;
  return false;
}

bool waitFor(Stream& in, const char* token, uint32_t timeoutMs) {
  char win[64] = {0};
  size_t len = strlen(token), i = 0;
  uint32_t t = millis();
  while (millis() - t < timeoutMs) {
    if (pollCancel()) return false;
    int c = in.read();
    if (c < 0) { delay(2); continue; }
    if (i < sizeof(win) - 1) { win[i++] = (char)c; } else { memmove(win, win + 1, sizeof(win) - 2); win[sizeof(win) - 2] = (char)c; }
    if (i >= len && strstr(win, token)) return true;
  }
  return false;
}

bool numberInRange(const char* input, double low, double high, double& value) {
  char* end;
  value = strtod(input, &end);
  return end != input && *end == 0 && isfinite(value) && value >= low && value <= high;
}

bool coordinate(const char* field, const char* hemi, bool latitude, float& result) {
  double raw;
  int limit = latitude ? 90 : 180;
  const char* sides = latitude ? "NS" : "EW";
  if (strlen(hemi) != 1 || !strchr(sides, hemi[0]) ||
      !numberInRange(field, 0, limit * 100, raw)) return false;
  int degrees = (int)(raw / 100);
  double minutes = raw - degrees * 100;
  if (minutes >= 60) return false;
  result = (degrees + minutes / 60) * (hemi[0] == sides[1] ? -1 : 1);
  return true;
}

bool checksumOK(const char* sentence) {
  const char* star = strchr(sentence, '*');
  if (!star || strlen(star + 1) != 2 || !isxdigit(star[1]) || !isxdigit(star[2])) return false;
  unsigned char check = 0;
  for (const char* p = sentence + 1; p < star; ++p) check ^= *p;
  return check == strtol(star + 1, nullptr, 16);
}

// Liga o GPS (partida a frio), espera a frase $GPRMC/$GNRMC válida e desliga o GPS.
bool acquireGps(float& lat, float& lon, uint32_t& ttffMs) {
  digitalWrite(LED_GNSS, HIGH);
  digitalWrite(PIN_GPS_PWR, HIGH);
#if GPS_ON_PC
  Serial.println("GPS_ON");
#endif
  uint32_t t = millis();
  bool ok = false;
  char line[100];
  while (millis() - t < GPS_TIMEOUT_S * 1000UL) {
    if (cancelRequested) break;
    if (!readLineFrom(gps, line, sizeof(line), 2000)) continue;
    char* rmc = strstr(line, "$GPRMC");
    if (!rmc) rmc = strstr(line, "$GNRMC");
    if (!rmc || !checksumOK(rmc)) continue;                      // ignora qualquer coisa antes da frase (ruído na serial)
    memmove(line, rmc, strlen(rmc) + 1);
    // $GPRMC,hhmmss,A,lat,N,lon,W,...  -> campos 2 (status), 3/4 (lat), 5/6 (lon)
    char* f[8] = {0};
    int k = 0;
    for (char* p = line; *p && k < 8; p++) {
      if (*p == ',') { *p = 0; f[k++] = p + 1; }
    }
    if (k < 6 || f[1] == nullptr || strcmp(f[1], "A") != 0) continue;
    if (!coordinate(f[2], f[3], true, lat) || !coordinate(f[4], f[5], false, lon)) continue;
    ok = true;
    break;
  }
  ttffMs = millis() - t;
  digitalWrite(PIN_GPS_PWR, LOW);           // corta o GPS até o próximo envio
#if GPS_ON_PC
  Serial.println("GPS_OFF");
#endif
  return ok;
}

// Envia um SMS pelo SIM800L com comandos AT padrão.
bool sendSms(const char* text) {
  digitalWrite(LED_GSM, HIGH);
  uint32_t t = millis();
  // Limpa o que sobrou na serial e sincroniza com o modem: o SIM800L em autobaud precisa de
  // alguns "AT" até responder, então tenta até 5 vezes.
  uint32_t tDrain = millis();
  while (gsm.available() && millis() - tDrain < 300) gsm.read();
  bool ok = false;
  for (int i = 0; i < 5 && !ok && !cancelRequested; i++) {
    gsm.print("AT\r\n");
    ok = waitFor(gsm, "OK", 1000);
  }
  if (ok) { gsm.print("AT+CMGF=1\r\n"); ok = waitFor(gsm, "OK", 2000); }
  if (ok) {
    gsm.print("AT+CMGS=\"" CONTACT_NUMBER "\"\r\n");
    ok = waitFor(gsm, ">", 5000);
  }
  if (ok) {
    gsm.print(text);
    gsm.write(0x1A);                         // Ctrl+Z envia a mensagem
    ok = waitFor(gsm, "+CMGS", 30000);
  }
  Serial.printf("\nSMS_%s,%lu,%lu,%lu\n", ok ? "OK" : "FAIL", (unsigned long)emergencyId,
                (unsigned long)sendCount, (unsigned long)(millis() - t));
  return ok;
}

// Monta o texto do SMS e envia.
bool smsPosition(float lat, float lon, const char* source) {
  char msg[160];
  snprintf(msg, sizeof(msg), "EMERGENCIA (%s). Localizacao: https://maps.google.com/?q=%.6f,%.6f", source, lat, lon);
  return sendSms(msg);
}

// ---- Caminho do celular ----
// Retorna true se o ACK chegou dentro de T_ACK_MS.
bool tryPhonePath(uint32_t t0) {
  setupBLE();
  ackReceived = false;
  ackPayload = "";
  tConnect = 0;

  uint32_t lastNotify = 0;
  while (millis() - t0 < T_ACK_MS && !ackReceived && !pollCancel()) {
    if (connected && millis() - lastNotify >= 500) {
      alertChar->setValue("ALERT");
      alertChar->notify();
      lastNotify = millis();
    }
    delay(10);
  }

  double lat, lon, acc;
  int first = ackPayload.indexOf(';');
  int second = ackPayload.indexOf(';', first + 1);
  if (ackReceived && first > 0 && second > first &&
      numberInRange(ackPayload.substring(0, first).c_str(), -90, 90, lat) &&
      numberInRange(ackPayload.substring(first + 1, second).c_str(), -180, 180, lon) &&
      numberInRange(ackPayload.substring(second + 1).c_str(), 0, 1e9, acc)) {
    unsigned long dt = millis() - t0;
    digitalWrite(LED_PHONE, HIGH);
    // RESULT,emergência,envio,t_desde_inicio_s,caminho,latência_ms,payload
    Serial.printf("RESULT,%lu,%lu,%.1f,celular,%lu,%s\n", (unsigned long)emergencyId,
                  (unsigned long)sendCount, elapsedS(), dt, safePayload(ackPayload).c_str());
    Serial.printf("MEAS,%lu,%lu,%lu,%lu\n", (unsigned long)emergencyId, (unsigned long)sendCount,
                  tConnect ? (unsigned long)(tConnect - t0) : 0UL, dt);
    lastLat = lat;
    lastLon = lon;
    haveLast = true;
    return true;
  } else {
    Serial.printf("NOACK,%lu,%lu,%.1f,sem resposta em %lu ms\n", (unsigned long)emergencyId,
                  (unsigned long)sendCount, elapsedS(), (unsigned long)T_ACK_MS);
  }
  return false;
}

// ---- Fallback: GPS e SIM800L do próprio botão ----
// Retorna true se o SMS saiu.
bool fallbackPath(uint32_t t0) {
  float lat = 0, lon = 0;
  uint32_t ttff = 0;
  bool fix = acquireGps(lat, lon, ttff);
  if (cancelRequested) return false;       // encerrada durante o GPS: não manda SMS
  const char* src = "GPS";
  if (fix) {
    lastLat = lat; lastLon = lon; haveLast = true;
  } else if (haveLast) {
    lat = lastLat; lon = lastLon; src = "ultima posicao conhecida";
  } else {
    src = "sem posicao";
  }
  Serial.printf("GPS_%s,%lu,%lu,%lu\n", fix ? "FIX" : "NOFIX", (unsigned long)emergencyId,
                (unsigned long)sendCount, (unsigned long)ttff);
  bool sent = haveLast ? smsPosition(lat, lon, src) : sendSms("EMERGENCIA. Localizacao indisponivel.");
  Serial.printf("RESULT,%lu,%lu,%.1f,fallback,%lu,%s\n", (unsigned long)emergencyId,
                (unsigned long)sendCount, elapsedS(), (unsigned long)(millis() - t0), fix ? "fix" : "nofix");
  return sent;
}

// ---- Um envio da localização ----
void sendLocation(uint32_t t0) {
  sendCount++;
  ledsOff();

  bool phonePosition = USE_PHONE_PATH && !bleFailed && tryPhonePath(t0);
  bool sent = false;
  if (!cancelRequested) {
    if (phonePosition) sent = smsPosition(lastLat, lastLon, "celular");
    else {
      bleFailed = true;
      sent = fallbackPath(t0);
    }
  }

  if (cancelRequested) {           // botão segurado 3 s no meio do envio: encerra aqui
    endEmergency();
    return;
  }
  if (sent) vibrate(1);            // 1 vibrada a cada envio bem-sucedido
  delay(1500);
  ledsOff();
  nextSendMs += (uint64_t)DELTA_S * 1000ULL;   // agenda pelo relógio, sem acumular atraso
}

// ---- Encerramento por botão segurado ----
// Chamado quando a placa acorda pelo botão durante uma emergência.
// Retorna true se o botão ficou pressionado por HOLD_MS.
bool heldToEnd() {
  uint32_t start = millis();
  while (digitalRead(PIN_BUTTON) == LOW) {
    if (millis() - start >= HOLD_MS) return true;
    delay(10);
  }
  return false;
}

// Começa uma emergência (pelo botão ou pela voz) e faz o primeiro envio.
void startEmergency(uint32_t t0, const char* source) {
  // Descarta o áudio que sobrou no buffer, por no máximo 300 ms (o PC para de mandar ao ver o KWS/START)
  uint32_t tDrain = millis();
  uint8_t junk[256];
  while (Serial.available() && millis() - tDrain < 300) {
    Serial.read(junk, min((int)sizeof(junk), Serial.available()));
  }
  emergencyActive = true;
  bleFailed = false;
  sendCount = 0;
  emergencyId++;
  startMs = nowMs();
  nextSendMs = startMs;
  Serial.printf("START,%lu,%s,delta_s=%lu,t_ack_ms=%lu\n", (unsigned long)emergencyId, source,
                (unsigned long)DELTA_S, (unsigned long)T_ACK_MS);
  sendLocation(t0);
}

void endEmergency() {
  emergencyActive = false;
  cancelRequested = false;
  bleFailed = false;
  Serial.printf("END,%lu,%lu,%.1f\n", (unsigned long)emergencyId, (unsigned long)sendCount, elapsedS());
  vibrate(2);                      // 2 vibradas: fim da emergência
}

// ---- Modo voz: o PC faz o papel do microfone e manda o áudio cru pela USB ----
// Cada pacote: 0xAA 0x55 + FRAME_SAMPLES amostras int16 (little-endian), 16 kHz, mono.
// O reconhecimento roda aqui na ESP32, com o modelo do Edge Impulse. Trocar o PC por um
// microfone I2S (ex.: INMP441) muda só a função readFrame().
#if ACTIVATION_MODE == 1
static int getAudio(size_t offset, size_t length, float* out) {
  ei::numpy::int16_to_float(&slice[offset], out, length);
  return 0;
}

// Lê um pacote; devolve false se o botão for apertado ou se o áudio parar de chegar.
bool readFrame(int16_t* dst) {
  uint32_t t = millis();
  int prev = -1;
  while (true) {
    if (digitalRead(PIN_BUTTON) == LOW) return false;
    if (millis() - t > 1000) return false;
    int b = Serial.read();
    if (b < 0) { delay(1); continue; }
    if (prev == 0xAA && b == 0x55) break;
    prev = b;
  }
  return Serial.readBytes((uint8_t*)dst, FRAME_SAMPLES * 2) == FRAME_SAMPLES * 2;
}

void startEmergency(uint32_t t0, const char* source);

// Fica ouvindo até a palavra secreta (ou o botão) e então começa a emergência.
void listenLoop() {
  Serial.printf("LISTEN,word=%s,freq=%d,slice=%d,frame=%d\n", KEYWORD, (int)EI_CLASSIFIER_FREQUENCY,
                (int)EI_CLASSIFIER_SLICE_SIZE, FRAME_SAMPLES);
  Serial.flush();                       // termina de enviar o texto antes de mudar o clock
  setCpuFrequencyMhz(LISTEN_CPU_MHZ);   // economia enquanto só ouve (ver simulation/energy_sim.py)
  run_classifier_init();
  signal_t sig;
  sig.total_length = EI_CLASSIFIER_SLICE_SIZE;
  sig.get_data = &getAudio;
  int hits = 0;
  int cooldown = 0;
  uint32_t lastAnnounce = millis();

  while (true) {
    if (millis() - lastAnnounce > 10000) {   // lembra o PC a cada 10 s que a placa está ouvindo
      Serial.println("LISTEN");
      lastAnnounce = millis();
    }
    if (digitalRead(PIN_BUTTON) == LOW) {
      delay(30);
      if (digitalRead(PIN_BUTTON) == LOW && !KWS_TEST_MODE) { startEmergency(millis(), "botao"); return; }
    }
    bool ok = true;
    for (int f = 0; f < EI_CLASSIFIER_SLICE_SIZE / FRAME_SAMPLES; f++) {
      if (!readFrame(&slice[f * FRAME_SAMPLES])) { ok = false; break; }
    }
    if (!ok) continue;

    ei_impulse_result_t r = {0};
    if (run_classifier_continuous(&sig, &r, false, true) != EI_IMPULSE_OK) continue;
    float score = 0;
    for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
      if (strcmp(r.classification[i].label, KEYWORD) == 0) score = r.classification[i].value;
    }
    if (cooldown > 0) { cooldown--; hits = 0; continue; }
    if (score >= KWS_THRESHOLD) {
      if (++hits >= KWS_HITS) {
        Serial.printf("KWS,%s,%.2f\n", KEYWORD, score);
#if KWS_TEST_MODE
        vibrate(1);                      // teste: só bipa e continua ouvindo
        Serial.println("LISTEN");        // avisa o PC para continuar mandando o áudio
        hits = 0;
        cooldown = 4;                    // ignora ~1 s para não contar a mesma palavra duas vezes
        continue;
#else
        startEmergency(millis(), "voz");
        return;
#endif
      }
    } else {
      hits = 0;
    }
  }
}
#endif

// ---- Pareamento ao ligar ----
// A página só reconecta sozinha depois de ter conectado uma vez. Ao ligar, a placa anuncia o
// Bluetooth por PAIR_S segundos para essa primeira conexão e depois reinicia limpa (deep sleep curto).
void pairWindow() {
  Serial.printf("PAIR,conecte a pagina agora (ate %lu s)\n", (unsigned long)PAIR_S);
  setupBLE();
  uint32_t t = millis();
  while (!connected && millis() - t < PAIR_S * 1000UL) delay(50);
  Serial.printf("PAIR,%s\n", connected ? "ok" : "sem conexao");
  delay(1500);                                   // deixa a página terminar de assinar o ALERT
  if (server && connected) { server->disconnect(server->getConnId()); delay(300); }
  esp_sleep_enable_timer_wakeup(200000ULL);      // volta em 0,2 s com o Bluetooth zerado
  Serial.flush();
  esp_deep_sleep_start();
}

// ---- Deep sleep ----
void goToSleep() {
#if ACTIVATION_MODE == 1
  if (!emergencyActive) {          // no modo voz a placa não dorme parada: fica ouvindo
    while (digitalRead(PIN_BUTTON) == LOW) delay(10);
    listenLoop();
    if (!emergencyActive) {
      while (digitalRead(PIN_BUTTON) == LOW) delay(10);
      esp_sleep_enable_timer_wakeup(200000ULL);
      Serial.flush();
      esp_deep_sleep_start();
    }
  }
#endif
  while (digitalRead(PIN_BUTTON) == LOW) delay(10);
  delay(50);
  if (server && connected) {
    server->disconnect(server->getConnId());
    delay(200);
  }
  rtc_gpio_pullup_en(GPIO_NUM_33);
  rtc_gpio_pulldown_dis(GPIO_NUM_33);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_33, 0);   // o botão sempre acorda a placa

  if (emergencyActive) {                          // durante a emergência, o timer também acorda
    uint64_t now = nowMs();
    uint64_t waitMs = (nextSendMs > now) ? (nextSendMs - now) : 100;
    esp_sleep_enable_timer_wakeup(waitMs * 1000ULL);
  }

  ledsOff();
  gpio_hold_en((gpio_num_t)LED_PHONE);
  gpio_hold_en((gpio_num_t)LED_GNSS);
  gpio_hold_en((gpio_num_t)LED_GSM);
  gpio_hold_en((gpio_num_t)PIN_VIBRA);
  gpio_hold_en((gpio_num_t)PIN_GPS_PWR);     // GPS continua sem alimentação durante o sono
  Serial.flush();
  esp_deep_sleep_start();
}

void setup() {
  uint32_t t0 = millis();
  Serial.setRxBufferSize(16384);   // folga para o áudio do modo voz
  Serial.begin(BAUD);
  Serial.println();                // fecha a linha de lixo que a ESP32 solta ao acordar

  rtc_gpio_deinit(GPIO_NUM_33);
  gpio_hold_dis((gpio_num_t)LED_PHONE);
  gpio_hold_dis((gpio_num_t)LED_GNSS);
  gpio_hold_dis((gpio_num_t)LED_GSM);
  gpio_hold_dis((gpio_num_t)PIN_VIBRA);
  gpio_hold_dis((gpio_num_t)PIN_GPS_PWR);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_VIBRA, OUTPUT);
  digitalWrite(PIN_VIBRA, LOW);
  pinMode(LED_PHONE, OUTPUT);
  pinMode(LED_GNSS, OUTPUT);
  pinMode(LED_GSM, OUTPUT);
  pinMode(PIN_GPS_PWR, OUTPUT);
  digitalWrite(PIN_GPS_PWR, LOW);
  ledsOff();
#if !GPS_ON_PC
  gps.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);   // NEO-6M: 9600 baud de fábrica
#endif
#if !GSM_ON_PC
  gsm.begin(9600, SERIAL_8N1, GSM_RX, GSM_TX);   // SIM800L: autobauding
#endif

#if ACTIVATION_MODE == 1 && KWS_TEST_MODE
  emergencyActive = false;
  Serial.println("TESTE DE VOZ: fale a palavra e aperte Enter no Terminal a cada vez; Ctrl+C mostra o resumo.");
  listenLoop();                    // no modo teste nunca volta
#endif
  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();

  if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    if (!emergencyActive) {
      // Aperto com a placa em repouso: começa uma emergência
      startEmergency(t0, "botao");
    } else if (heldToEnd()) {
      // Botão segurado por HOLD_MS durante a emergência: encerra
      endEmergency();
    }
    // Aperto curto durante a emergência: ignorado; volta a dormir até o próximo envio
  } else if (cause == ESP_SLEEP_WAKEUP_TIMER && emergencyActive) {
    sendLocation(t0);              // envio periódico
  } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
    Serial.println("PRONTO. Aperte o botao (ou fale a palavra) para acionar; segure 3 s para encerrar.");
    vibrate(1);                    // 1 bipe ao ficar pronta: confirma que o buzzer/motor está ligado
  } else {
    // Ligou ou foi resetada: começa em repouso e abre a janela de pareamento
    emergencyActive = false;
    bleFailed = false;
#if USE_PHONE_PATH
    pairWindow();                  // não volta: reinicia por deep sleep curto
#else
    Serial.println("PRONTO (so hardware: GPS + SMS). Aperte o botao (ou fale a palavra); segure 3 s para encerrar.");
#endif
  }

  goToSleep();
}

void loop() {}
