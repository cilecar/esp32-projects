#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>

const int RELAY_PIN = 2;

// Явно задаем тот MAC, который ждет хаб
uint8_t customMac[] = {0xE8, 0x3D, 0xC1, 0x9C, 0xBE, 0xE4};

// MAC хаба — должен совпадать с hubMac в hub-code
uint8_t hubMac[] = {0x34, 0x85, 0x18, 0x01, 0x02, 0x03};

enum { MSG_CMD = 1, MSG_ANNOUNCE = 2, MSG_REPORT = 3 };
typedef struct { uint8_t type; uint8_t value; } msg_t;

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len >= (int)sizeof(msg_t) && data[0] == MSG_CMD) {
    digitalWrite(RELAY_PIN, data[1] ? HIGH : LOW);
  }
}

void announce() {
  msg_t m{MSG_ANNOUNCE, 0};
  for (int i = 0; i < 3; i++) { esp_now_send(hubMac, (uint8_t*)&m, sizeof(m)); delay(80); }
}

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  WiFi.setSleep(false);

  // Принудительно ставим MAC-адрес, чтобы исключить ошибку STA/AP интерфейсов
  esp_wifi_set_mac(WIFI_IF_STA, &customMac[0]);

  // Снижаем мощность передатчика, чтобы повербанк не захлебывался
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(11, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(onDataRecv);

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, hubMac, 6);
    peer.channel = 11;
    peer.encrypt = false;
    esp_now_add_peer(&peer);

    announce();   // запрос актуального состояния у хаба при загрузке
  }
}

void loop() {
  delay(10); // Отдаем время планировщику RTOS
}
