#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

const uint8_t PIN_RELAY = 4;
const uint8_t PIN_LED = 6;
const uint8_t PIN_BUTTON = 9;

// MAC хаба — должен совпадать с hubMac в hub-code
uint8_t hubMac[] = {0x34, 0x85, 0x18, 0x01, 0x02, 0x03};

enum { MSG_CMD = 1, MSG_ANNOUNCE = 2, MSG_REPORT = 3 };
typedef struct { uint8_t type; uint8_t value; } msg_t;

bool relayState = false;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;

void setRelay(bool state) {
  relayState = state;
  digitalWrite(PIN_RELAY, relayState ? HIGH : LOW);
  digitalWrite(PIN_LED, relayState ? LOW : HIGH);
}

// Сообщить хабу текущее состояние (после физической кнопки)
void report() {
  msg_t m{MSG_REPORT, (uint8_t)(relayState ? 1 : 0)};
  esp_now_send(hubMac, (uint8_t*)&m, sizeof(m));
}

// Сообщить хабу о загрузке — он пришлёт актуальное состояние
void announce() {
  msg_t m{MSG_ANNOUNCE, 0};
  for (int i = 0; i < 3; i++) { esp_now_send(hubMac, (uint8_t*)&m, sizeof(m)); delay(80); }
}

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len >= (int)sizeof(msg_t) && data[0] == MSG_CMD) {
    setRelay(data[1] == 1);
  }
}

void setup() {
  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  setRelay(false);

  WiFi.mode(WIFI_STA);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(11, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() != ESP_OK) ESP.restart();
  esp_now_register_recv_cb(onDataRecv);

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, hubMac, 6);
  peer.channel = 11;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  announce();   // запрос актуального состояния у хаба при подключении к сети
}

void loop() {
  bool reading = digitalRead(PIN_BUTTON);
  static bool lastButtonState = HIGH;

  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }

  if ((millis() - lastDebounceTime) > debounceDelay) {
    static bool buttonState = HIGH;
    if (reading != buttonState) {
      buttonState = reading;
      if (buttonState == LOW) {
        setRelay(!relayState);
        report();   // держим хаб в курсе физического переключения
      }
    }
  }
  lastButtonState = reading;
}
