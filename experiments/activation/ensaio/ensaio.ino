// Ensaio de acionamento com rearme automático. Não envia alertas externos.
#include <Arduino.h>
#define EI_CLASSIFIER_SLICES_PER_MODEL_WINDOW 4
#define EIDSP_QUANTIZE_FILTERBANK 0
#include <ticiive-project-2_inferencing.h>
const int PIN_BUTTON = 33;
const int FRAME_SAMPLES = 500;
static_assert(EI_CLASSIFIER_SLICE_SIZE % FRAME_SAMPLES == 0, "Tamanho de pacote incompatível");
static int16_t slice[EI_CLASSIFIER_SLICE_SIZE];
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


signal_t signalAudio;
bool buttonLatched = false;
int consecutive = 0;
int cooldown = 0;
unsigned long lastHeartbeat = 0;
unsigned long slicesProcessed = 0;

void setup() {
  Serial.setRxBufferSize(16384);
  setCpuFrequencyMhz(160);
  Serial.begin(460800);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  run_classifier_init();
  signalAudio.total_length = EI_CLASSIFIER_SLICE_SIZE;
  signalAudio.get_data = getAudio;
  Serial.println("READY");
}

void loop() {
  if (millis() - lastHeartbeat >= 1000) {
    Serial.printf("ALIVE,%lu\n", slicesProcessed);
    lastHeartbeat = millis();
  }
  if (digitalRead(PIN_BUTTON) == LOW) {
    if (!buttonLatched) {
      delay(30);
      if (digitalRead(PIN_BUTTON) == LOW) {
        Serial.println("START,botao");
        buttonLatched = true;
      }
    }
    consecutive = 0;
    return;
  }
  buttonLatched = false;
  for (int frame = 0; frame < EI_CLASSIFIER_SLICE_SIZE / FRAME_SAMPLES; ++frame) {
    if (!readFrame(slice + frame * FRAME_SAMPLES)) {
      consecutive = 0;
      return;
    }
  }
  ei_impulse_result_t result = {};
  if (run_classifier_continuous(&signalAudio, &result, false, true) != EI_IMPULSE_OK) {
    consecutive = 0;
    Serial.println("ERROR,inference");
    return;
  }
  ++slicesProcessed;
  if (cooldown > 0) { --cooldown; return; }
  float score = 0;
  for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; ++i)
    if (strcmp(result.classification[i].label, "abacaxi") == 0)
      score = result.classification[i].value;
  consecutive = score >= 0.80f ? consecutive + 1 : 0;
  if (consecutive >= 2) {
    Serial.println("START,voz");
    consecutive = 0;
    cooldown = 4;
  }
}
