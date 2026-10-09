#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

constexpr uint8_t PIN_LDR = 0;
constexpr uint8_t PIN_OPTO = 3;
constexpr uint32_t LDR_THRESHOLD = 4000;

// MAC хаба — должен совпадать с hubMac в hub-code
uint8_t hubMac[] = {0x34, 0x85, 0x18, 0x01, 0x02, 0x03};

enum { MSG_CMD = 1, MSG_ANNOUNCE = 2, MSG_REPORT = 3 };
typedef struct { uint8_t type; uint8_t value; } msg_t;

volatile bool hasNewCommand = false;
volatile uint8_t receivedCommand = 0;

// Пока хаб не ответил ни разу — повторяем announce
bool hubAcked = false;
unsigned long lastAnnounce = 0;
const unsigned long ANNOUNCE_INTERVAL = 10000;

bool isLightOn() {
  uint32_t sum = 0;
  for (int i = 0; i < 10; i++) {
    sum += analogRead(PIN_LDR);
    delay(10);
  }
  uint32_t avg = sum / 10;
  return avg > LDR_THRESHOLD;
}

void clickOpto() {
  digitalWrite(PIN_OPTO, HIGH);
  delay(100);
  digitalWrite(PIN_OPTO, LOW);
  delay(500);
}

void turnOffLight() {
  int attempts = 0;
  while (isLightOn() && attempts < 4) {
    clickOpto();
    attempts++;
  }
}

void turnOnLight(int targetClicks) {
  turnOffLight(); // Гарантированный сброс в 0
  for (int i = 0; i < targetClicks; i++) {
    clickOpto();
  }
}

void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len >= (int)sizeof(msg_t) && data[0] == MSG_CMD) {
    receivedCommand = data[1];   // 0 = включить, 1 = выключить
    hasNewCommand = true;
    hubAcked = true;             // хаб на связи — прекращаем повторы announce
  }
}

void sendAnnounce() {
  msg_t m{MSG_ANNOUNCE, 0};
  esp_now_send(hubMac, (uint8_t*)&m, sizeof(m));
}

void setup() {
  pinMode(PIN_OPTO, OUTPUT);
  digitalWrite(PIN_OPTO, LOW);
  analogReadResolution(12);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(11, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(OnDataRecv);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, hubMac, 6);
    peer.channel = 11;
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    sendAnnounce();          // первый запрос состояния сразу
    lastAnnounce = millis();
  }
}

void loop() {
  // Повторяем announce, пока хаб хоть раз не ответит командой
  if (!hubAcked && millis() - lastAnnounce > ANNOUNCE_INTERVAL) {
    lastAnnounce = millis();
    sendAnnounce();
  }

  if (hasNewCommand) {
    uint8_t command = receivedCommand;
    hasNewCommand = false;

    bool lightCurrentlyOn = isLightOn();

    if (command == 0) {            // Хаб: Включить ночник
      if (!lightCurrentlyOn) turnOnLight(2);
    } else if (command == 1) {     // Хаб: Выключить ночник
      if (lightCurrentlyOn) turnOffLight();
    }
  }
  delay(10);
}
