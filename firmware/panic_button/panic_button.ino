#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "driver/rtc_io.h"
#include "driver/gpio.h"

// ---------------- Configuração ----------------
#define USE_DEEP_SLEEP 0        // 1 = dorme entre acionamentos; 0 = fica acordada e conectada
const uint32_t T_MS = 15000;    // tempo limite de espera pelo ACK do celular (T = 15 s)

// Pinos
const int PIN_BUTTON = 33;      // botão; a outra perna (diagonal) vai no GND
const int PIN_BUZZER = 32;
const int LED_PHONE  = 13;      // LED azul: caminho do celular (ACK recebido)
const int LED_GNSS   = 27;      // LED vermelho: GPS do chaveiro (simulado)
const int LED_GSM    = 26;      // LED vermelho: SMS do chaveiro (simulado)

// UUIDs (padrão Nordic UART)
#define SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define ACK_UUID     "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  // celular -> ESP32 (write)
#define ALERT_UUID   "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  // ESP32 -> celular (notify)

RTC_DATA_ATTR int trial = 0;    // contador que sobrevive ao deep sleep

BLECharacteristic* alertChar = nullptr;
volatile bool connected = false;
volatile bool ackReceived = false;
String ackPayload = "";

class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override { connected = true; }
  void onDisconnect(BLEServer* s) override {
    connected = false;
    BLEDevice::startAdvertising();
  }
};

class AckCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    ackPayload = String(c->getValue().c_str());   // esperado: "lat;lon;acc"
    ackReceived = true;
  }
};

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

void setupBLE() {
  BLEDevice::init("PanicButton");
  BLEServer* server = BLEDevice::createServer();
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

void runAlertCycle(uint32_t t0) {
  trial++;
  ackReceived = false;
  ackPayload = "";
  ledsOff();
  beep(150);

  uint32_t lastNotify = 0;
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

void goToSleep() {
  while (digitalRead(PIN_BUTTON) == LOW) delay(10);
  delay(50);
  rtc_gpio_pullup_en(GPIO_NUM_33);
  rtc_gpio_pulldown_dis(GPIO_NUM_33);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_33, 0);
  ledsOff();
  gpio_hold_en((gpio_num_t)LED_PHONE);
  gpio_hold_en((gpio_num_t)LED_GNSS);
  gpio_hold_en((gpio_num_t)LED_GSM);
  Serial.flush();
  esp_deep_sleep_start();
}

void setup() {
  uint32_t t0 = millis();
  Serial.begin(115200);

  rtc_gpio_deinit(GPIO_NUM_33);
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
