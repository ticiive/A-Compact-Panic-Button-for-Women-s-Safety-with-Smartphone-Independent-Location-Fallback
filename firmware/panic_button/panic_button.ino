#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "driver/rtc_io.h"
#include "driver/gpio.h"

// ---------------- Configuração ----------------
#define USE_DEEP_SLEEP 1       // 1 = dorme entre acionamentos; 0 = fica acordada e conectada
#define MEASURE_MODE   1       // 1 = T de 60 s para medir reconexão sem cair no fallback; 0 = T de 15 s (produção)
const uint32_t T_MS = MEASURE_MODE ? 60000 : 15000;  // 15 s é o T do método; 60 s só para medir a reconexão

// Pinos
const int PIN_BUTTON = 33;      // botão; a outra perna (diagonal) vai no GND
const int PIN_BUZZER = 32;
const int LED_PHONE  = 13;      // LED azul: caminho do celular (ACK recebido)
const int LED_GNSS   = 27;      // LED vermelho: GPS do chaveiro (simulado)
const int LED_GSM    = 26;      // LED vermelho: SMS do chaveiro (simulado)

// UUIDs do serviço Nordic UART (reutilizados com semântica própria ALERT/ACK)
#define SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define ACK_UUID     "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  // celular -> ESP32 (write)
#define ALERT_UUID   "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  // ESP32 -> celular (notify)

RTC_DATA_ATTR int trial = 0;    // contador de tentativas; sobrevive ao deep sleep via RTC RAM

// Globais BLE: precisam ser acessíveis fora de setupBLE()
BLECharacteristic* alertChar = nullptr;
BLEServer* server = nullptr;
volatile bool connected = false;
volatile bool ackReceived = false;
volatile uint32_t tConnect = 0;  // instante da primeira conexão desde o wake-up (millis())
String ackPayload = "";

// ---- Callbacks do servidor BLE ----
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override {
    connected = true;
    if (tConnect == 0) tConnect = millis();  // registra apenas a primeira conexão do ciclo
  }
  void onDisconnect(BLEServer* s) override {
    connected = false;
    BLEDevice::startAdvertising();  // retoma o anúncio para permitir reconexão
  }
};

// Callback da característica ACK: celular escreve "lat;lon;acc" para confirmar o envio
class AckCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    ackPayload = String(c->getValue().c_str());
    ackReceived = true;
  }
};

// ---- Utilitários ----
void beep(int ms) {
  digitalWrite(PIN_BUZZER, HIGH);
  delay(ms);
  digitalWrite(PIN_BUZZER, LOW);
}

void ledsOff() {
  digitalWrite(LED_PHONE, LOW);
  digitalWrite(LED_GNSS, LOW);
  digitalWrite(LED_GSM, LOW);
}

// ---- Inicialização BLE ----
// Cria servidor GATT com serviço Nordic UART, característica ALERT (notify)
// e característica ACK (write). O ponteiro global server é usado em goToSleep().
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

// ---- Ciclo de alerta ----
// Reenvia notificação ALERT a cada 500 ms até receber ACK ou esgotar T_MS.
// Se ACK chegar: acende LED azul, imprime RESULT e MEAS (instrumentação).
// Se não chegar: aciona fallback (LED do GPS, depois LED do GSM).
void runAlertCycle(uint32_t t0) {
  trial++;
  ackReceived = false;
  ackPayload = "";
  tConnect = 0;
  ledsOff();
  beep(150);

  uint32_t lastNotify = 0;
  // Reenvia a cada 500 ms: o celular pode não ter subscrito na primeira notificação
  while (millis() - t0 < T_MS && !ackReceived) {
    if (connected && millis() - lastNotify >= 500) {
      alertChar->setValue("ALERT");
      alertChar->notify();
      lastNotify = millis();
    }
    delay(10);
  }

  const char* mode = USE_DEEP_SLEEP ? "deepsleep" : "acordada";
  if (ackReceived) {
    unsigned long dt = millis() - t0;
    digitalWrite(LED_PHONE, HIGH);
    Serial.printf("RESULT,%d,%s,celular,%lu,%s\n", trial, mode, dt, ackPayload.c_str());
    // Instrumentação: tempos contados a partir do wake-up (t0 = millis() no início do setup)
    Serial.printf("MEAS,%d,%lu,%lu\n", trial,
                  tConnect ? (unsigned long)(tConnect - t0) : 0UL, dt);
  } else {
    digitalWrite(LED_GNSS, HIGH);
    delay(1000);
    digitalWrite(LED_GSM, HIGH);
    Serial.printf("RESULT,%d,%s,fallback,-,-\n", trial, mode);
  }

  beep(150);
  delay(3000);
  ledsOff();
}

// ---- Deep sleep ----
// Aguarda o botão ser solto, encerra a conexão BLE explicitamente (para que o
// celular detecte a queda em ~3-4 s em vez de dezenas de segundos), configura
// wake-up por ext0 no GPIO33, congela os LEDs e dorme.
void goToSleep() {
  while (digitalRead(PIN_BUTTON) == LOW) delay(10);
  delay(50);
  // Desconexão explícita: sem ela, a queda era detectada pelo celular com atraso variável
  if (server && connected) {
    server->disconnect(server->getConnId());
    delay(200);
  }
  // Pull-up RTC obrigatório: o pull-up comum é desligado durante o sono
  rtc_gpio_pullup_en(GPIO_NUM_33);
  rtc_gpio_pulldown_dis(GPIO_NUM_33);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_33, 0);
  ledsOff();
  // gpio_hold congela o estado dos pinos para evitar LEDs semiacesos durante o sono
  gpio_hold_en((gpio_num_t)LED_PHONE);
  gpio_hold_en((gpio_num_t)LED_GNSS);
  gpio_hold_en((gpio_num_t)LED_GSM);
  Serial.flush();
  esp_deep_sleep_start();
}

void setup() {
  uint32_t t0 = millis();
  Serial.begin(115200);

  // Libera o pino do botão (pode ter ficado no modo RTC do boot anterior)
  rtc_gpio_deinit(GPIO_NUM_33);
  // Libera o hold dos LEDs imposto antes do sleep
  gpio_hold_dis((gpio_num_t)LED_PHONE);
  gpio_hold_dis((gpio_num_t)LED_GNSS);
  gpio_hold_dis((gpio_num_t)LED_GSM);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(LED_PHONE, OUTPUT);
  pinMode(LED_GNSS, OUTPUT);
  pinMode(LED_GSM, OUTPUT);
  ledsOff();

#if USE_DEEP_SLEEP
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    setupBLE();
    runAlertCycle(t0);
  } else {
    Serial.println("Ligada (modo deep sleep). Aperte o botao para acionar.");
  }
  goToSleep();
#else
  setupBLE();
  Serial.println("Ligada (modo acordada). Aperte o botao para acionar.");
#endif
}

void loop() {
#if !USE_DEEP_SLEEP
  if (digitalRead(PIN_BUTTON) == LOW) {
    delay(30);
    if (digitalRead(PIN_BUTTON) == LOW) {
      runAlertCycle(millis());
      while (digitalRead(PIN_BUTTON) == LOW) delay(10);
    }
  }
#endif
}
