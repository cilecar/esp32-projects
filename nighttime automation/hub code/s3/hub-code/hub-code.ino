#include "secrets.h"
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WakeOnLan.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <Preferences.h>
#include <WebServer.h>
#include <time.h>
#include <LittleFS.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// --- НАСТРОЙКИ СЕТИ И УСТРОЙСТВ ---
const uint8_t bedroomMac[]  = {0xE8, 0x3D, 0xC1, 0x9C, 0xBE, 0xE4};
const uint8_t kitchenMac[]  = {0x80, 0xF1, 0xB2, 0x64, 0xC9, 0x80};
const uint8_t corridorMac[] = {0xAC, 0xEB, 0xE6, 0x6B, 0xE5, 0xBC};

uint8_t hubMac[] = {0x34, 0x85, 0x18, 0x01, 0x02, 0x03};

const char* ssid     = WIFI_SSID;
const char* password = WIFI_PASS;
const char* pcUrl    = "http://192.168.31.31:5000/sleep";
const char* pcMAC    = PC_MAC;

// --- ПИНЫ ---
constexpr uint8_t PIN_IR  = 8;
constexpr uint8_t PIN_LED = 4;   // статусный светодиод

// I2C для OLED
constexpr uint8_t PIN_SDA = 1;
constexpr uint8_t PIN_SCL = 2;

// Кнопки
constexpr uint8_t BTN_ROOM    = 5;
constexpr uint8_t BTN_KITCHEN = 6;
constexpr uint8_t BTN_NIGHT   = 7;
constexpr uint8_t BTN_TV      = 11;
constexpr uint8_t BTN_PC      = 12;
constexpr uint8_t BTN_MASTER  = 13;  // короткое = всё вкл, долгое (>1с) = сон

constexpr uint8_t BTNS[]    = {BTN_ROOM, BTN_KITCHEN, BTN_NIGHT, BTN_TV, BTN_PC, BTN_MASTER};
constexpr uint8_t BTN_COUNT = sizeof(BTNS);

constexpr unsigned long DEBOUNCE_MS   = 50;
constexpr unsigned long LONG_PRESS_MS = 1000;

// --- OLED ---
#define SCREEN_W 128
#define SCREEN_H 64
#define OLED_ADDR 0x3C   // большинство модулей 0.96" — если не работает, попробуй 0x3D

Adafruit_SSD1306 display(SCREEN_W, SCREEN_H, &Wire, -1);

// --- ТАЙМАУТЫ ПАНЕЛИ ---
constexpr unsigned long PANEL_SLEEP_MS = 2UL * 60 * 1000;  // 2 мин без активности
constexpr unsigned long DISP_REFRESH_MS = 30000;             // обновление экрана раз в 30 сек

// --- СОСТОЯНИЯ ---
uint8_t stateRoom    = 1;
uint8_t stateKitchen = 0;
uint8_t stateNight   = 0;
uint8_t stateTV      = 0;   // предполагаемое состояние ТВ
uint8_t statePC      = 0;

// Сон/пробуждение панели
enum PanelState : uint8_t { PANEL_SLEEP = 0, PANEL_AWAKE = 1 };
PanelState panelState    = PANEL_SLEEP;
unsigned long lastActivityMs = 0;

// --- КНОПКИ: отслеживание ---
struct BtnState {
  bool     lastRaw;
  bool     pressed;
  unsigned long pressedAt;
  bool     longFired;
};
BtnState btn[BTN_COUNT];

IRsend irsend(PIN_IR);
WiFiUDP UDP;
WakeOnLan WOL(UDP);
Preferences preferences;
WebServer server(80);

// --- ПРОТОКОЛ ESP-NOW ---
enum { MSG_CMD = 1, MSG_ANNOUNCE = 2, MSG_REPORT = 3 };
typedef struct { uint8_t type; uint8_t value; } msg_t;

// --- СИСТЕМНЫЕ ПЕРЕМЕННЫЕ ---
const int LOG_SIZE = 50;
String logBuffer[LOG_SIZE];
int logIndex = 0;
bool logWrapped = false;

volatile bool espNowBedSuccess = false;
volatile bool espNowKitSuccess = false;
volatile bool espNowCorSuccess = false;

volatile bool annRoom = false, annKitchen = false, annNight = false;
volatile bool kitchenReported = false;
volatile uint8_t kitchenReportedState = 0;

unsigned long lastHeartbeatTime = 0;
unsigned long lastDispRefreshMs = 0;
int networkFailures = 0;
bool espNowReady = false;

void txCmd(const uint8_t* mac, uint8_t value) {
  msg_t m; m.type = MSG_CMD; m.value = value;
  esp_now_send(mac, (uint8_t*)&m, sizeof(m));
}

// --- ЛОГИРОВАНИЕ ---
String getTimeString() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return String(millis());
  char buf[20];
  strftime(buf, sizeof(buf), "%H:%M:%S", &timeinfo);
  return String(buf);
}

void sendLog(String message) {
  String fullMsg = "[" + getTimeString() + "] " + message;
  Serial.println(fullMsg);

  logBuffer[logIndex] = fullMsg;
  logIndex = (logIndex + 1) % LOG_SIZE;
  if (logIndex == 0) logWrapped = true;

  File file = LittleFS.open("/log.txt", FILE_APPEND);
  if (file) {
    file.print(fullMsg + "\n");
    if (file.size() > 4096) {
      file.close();
      file = LittleFS.open("/log.txt", FILE_WRITE);
      int start = logWrapped ? logIndex : 0;
      int count = logWrapped ? LOG_SIZE : logIndex;
      for (int i = 0; i < count; i++) {
        int idx = (start + i) % LOG_SIZE;
        file.print(logBuffer[idx] + "\n");
      }
    }
    file.close();
  }
}

// --- ДИСПЛЕЙ ---
// Рисует одну строку устройства: название слева, статус справа
// Статус ON: белый прямоугольник с чёрным текстом; OFF: просто текст
static void drawDevRow(const char* name, bool on, int y) {
  display.setCursor(0, y);
  display.setTextColor(SSD1306_WHITE);
  display.print(name);

  if (on) {
    display.fillRect(SCREEN_W - 25, y - 1, 25, 10, SSD1306_WHITE);
    display.setTextColor(SSD1306_BLACK);
    display.setCursor(SCREEN_W - 23, y);
    display.print(" ON");
    display.setTextColor(SSD1306_WHITE);
  } else {
    display.setCursor(SCREEN_W - 25, y);
    display.print("OFF");
  }
}

void updateDisplay() {
  if (panelState == PANEL_SLEEP) {
    display.ssd1306_command(SSD1306_DISPLAYOFF);
    return;
  }

  display.ssd1306_command(SSD1306_DISPLAYON);
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Шапка: название + время
  display.setCursor(0, 0);
  display.print("S3 Hub");
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    char buf[6];
    strftime(buf, sizeof(buf), "%H:%M", &timeinfo);
    display.setCursor(SCREEN_W - 5 * 6, 0);  // 5 символов × 6px
    display.print(buf);
  }

  // Разделитель
  display.drawFastHLine(0, 10, SCREEN_W, SSD1306_WHITE);

  // Строки устройств (y: 13, 23, 33, 43, 53)
  drawDevRow("Light",  stateRoom,    13);
  drawDevRow("Kitchen", stateKitchen, 23);
  drawDevRow("Night",  stateNight,   33);
  drawDevRow("TV",     stateTV,      43);
  drawDevRow("PC",     statePC,      53);

  display.display();
}

// --- ДЕЙСТВИЯ ---
void applyRoom(bool on) {
  stateRoom = on ? 1 : 0;
  preferences.putUInt("state", stateRoom);
  if (espNowReady) txCmd(bedroomMac, stateRoom);
  updateDisplay();
}

void applyKitchen(bool on) {
  stateKitchen = on ? 1 : 0;
  preferences.putUInt("k", stateKitchen);
  if (espNowReady) txCmd(kitchenMac, stateKitchen);
  updateDisplay();
}

void applyNight(bool on) {
  stateNight = on ? 1 : 0;
  preferences.putUInt("n", stateNight);
  if (espNowReady) txCmd(corridorMac, stateNight ? 0 : 1); // инверсия: 0=вкл
  updateDisplay();
}

void applyTV() {
  stateTV = stateTV ? 0 : 1;
  irsend.sendSony(0xA90, 12, 10);
  updateDisplay();
}

void applyPC(bool wake) {
  statePC = wake ? 1 : 0;
  if (wake) {
    for (int i = 0; i < 3; i++) { WOL.sendMagicPacket(pcMAC); delay(50); }
  } else {
    HTTPClient http; http.begin(pcUrl); http.setConnectTimeout(800); http.GET(); http.end();
  }
  updateDisplay();
}

void applyMaster(bool on) {
  applyKitchen(on);
  applyRoom(on);
  applyNight(!on);   // мастер вкл → ночник выкл, и наоборот
  applyTV();
  applyPC(on);
}

// --- ВЕБ-ИНТЕРФЕЙС ---
const char PANEL_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>S3 Hub</title><style>
body{background:#0e0e12;color:#e6e6e6;font-family:system-ui,sans-serif;margin:0;padding:16px}
h2{font-weight:600;margin:4px 0 14px}
.grid{display:grid;grid-template-columns:repeat(2,1fr);gap:10px;max-width:480px}
.tgl,.btn{padding:18px;border-radius:14px;text-align:center;font-size:16px;
  background:#1c1c24;border:1px solid #2a2a36;cursor:pointer;user-select:none;transition:.15s}
.tgl.on{background:#2563eb;border-color:#3b82f6;box-shadow:0 0 12px #2563eb66}
.btn:active{transform:scale(.97)}
.btn.warn{background:#7c2d12}
.full{grid-column:span 2}
pre{background:#000;color:#3ad94a;font-size:12px;padding:12px;border-radius:10px;
  margin-top:16px;max-height:40vh;overflow:auto;white-space:pre-wrap;max-width:480px}
</style></head><body>
<h2>S3 Hub</h2>
<div class="grid">
  <div id="room"    class="tgl" onclick="tog('room')">Лента</div>
  <div id="kitchen" class="tgl" onclick="tog('kitchen')">Кухня</div>
  <div id="night"   class="tgl" onclick="tog('night')">Ночник</div>
  <div class="btn" onclick="act('tv',0)">ТВ</div>
  <div id="pc" class="tgl" onclick="tog('pc')">ПК</div>
  <div class="btn full" onclick="act('master',1)">Всё включить</div>
  <div class="btn full warn" onclick="act('master',0)">Сон (всё выкл)</div>
</div>
<pre id="log">загрузка…</pre>
<script>
function act(d,v){fetch('/api/action?dev='+d+'&val='+v).then(refresh)}
function tog(id){var on=document.getElementById(id).classList.contains('on');act(id,on?0:1)}
function set(id,on){document.getElementById(id).classList.toggle('on',!!on)}
function refresh(){fetch('/api/state').then(r=>r.json()).then(s=>{set('room',s.room);set('kitchen',s.kitchen);set('night',s.night);set('pc',s.pc)})}
function logs(){fetch('/api/logs').then(r=>r.text()).then(t=>{document.getElementById('log').textContent=t})}
setInterval(refresh,2000);setInterval(logs,3000);refresh();logs();
</script></body></html>
)HTML";

void handleControl() { server.send_P(200, "text/html", PANEL_HTML); }

void handleState() {
  String j = "{\"room\":" + String(stateRoom) +
             ",\"kitchen\":" + String(stateKitchen) +
             ",\"night\":" + String(stateNight) +
             ",\"pc\":" + String(statePC) + "}";
  server.send(200, "application/json", j);
}

void handleAction() {
  String d = server.arg("dev");
  int v = server.arg("val").toInt();
  if      (d == "room")    applyRoom(v);
  else if (d == "kitchen") applyKitchen(v);
  else if (d == "night")   applyNight(v);
  else if (d == "tv")      applyTV();
  else if (d == "pc")      applyPC(v);
  else if (d == "master")  applyMaster(v);
  server.send(200, "text/plain", "ok");
}

void handleLogs() {
  String out;
  int start = logWrapped ? logIndex : 0;
  int count = logWrapped ? LOG_SIZE : logIndex;
  for (int i = 0; i < count; i++) {
    int idx = (start + i) % LOG_SIZE;
    out += logBuffer[idx] + "\n";
  }
  server.send(200, "text/plain", out);
}

// --- ESP-NOW ---
void onDataSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  bool success = (status == ESP_NOW_SEND_SUCCESS);
  if (memcmp(mac_addr, bedroomMac, 6) == 0)       espNowBedSuccess = success;
  else if (memcmp(mac_addr, kitchenMac, 6) == 0)  espNowKitSuccess = success;
  else if (memcmp(mac_addr, corridorMac, 6) == 0) espNowCorSuccess = success;
}

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  const uint8_t *mac = info->src_addr;
  if (len < (int)sizeof(msg_t)) return;
  uint8_t type = data[0], val = data[1];
  bool isRoom  = memcmp(mac, bedroomMac, 6) == 0;
  bool isKit   = memcmp(mac, kitchenMac, 6) == 0;
  bool isNight = memcmp(mac, corridorMac, 6) == 0;

  if (type == MSG_ANNOUNCE) {
    if (isRoom)       annRoom = true;
    else if (isKit)   annKitchen = true;
    else if (isNight) annNight = true;
  } else if (type == MSG_REPORT && isKit) {
    kitchenReported = true;
    kitchenReportedState = val;
  }
}

// --- КНОПКИ ---
void initButtons() {
  for (uint8_t i = 0; i < BTN_COUNT; i++) {
    pinMode(BTNS[i], INPUT_PULLUP);
    btn[i] = {true, false, 0, false};
  }
}

void onButtonClick(uint8_t idx) {
  if (panelState == PANEL_SLEEP) {
    // первое нажатие — только пробуждение, без действия
    panelState = PANEL_AWAKE;
    lastActivityMs = millis();
    updateDisplay();
    return;
  }

  lastActivityMs = millis();
  switch (idx) {
    case 0: applyRoom(!stateRoom);       break;
    case 1: applyKitchen(!stateKitchen); break;
    case 2: applyNight(!stateNight);     break;
    case 3: applyTV();                   break;
    case 4: applyPC(!statePC);           break;
    case 5: applyMaster(true);           break;
  }
}

void onButtonLong(uint8_t idx) {
  if (idx == 5) {
    lastActivityMs = millis();
    applyMaster(false);
  }
}

void handleButtons() {
  unsigned long now = millis();
  for (uint8_t i = 0; i < BTN_COUNT; i++) {
    bool raw = !digitalRead(BTNS[i]);

    if (raw != btn[i].lastRaw) {
      btn[i].lastRaw = raw;
    }

    if (raw && !btn[i].pressed) {
      btn[i].pressed   = true;
      btn[i].pressedAt = now;
      btn[i].longFired = false;
    }

    if (btn[i].pressed && raw && !btn[i].longFired) {
      if (now - btn[i].pressedAt >= LONG_PRESS_MS) {
        btn[i].longFired = true;
        onButtonLong(i);
      }
    }

    if (!raw && btn[i].pressed) {
      btn[i].pressed = false;
      if (!btn[i].longFired && (now - btn[i].pressedAt >= DEBOUNCE_MS)) {
        onButtonClick(i);
      }
    }
  }
}

// --- SETUP ---
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);

  irsend.begin();
  initButtons();

  Wire.begin(PIN_SDA, PIN_SCL);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("SSD1306 init failed");
  }
  display.setRotation(2);
  display.clearDisplay();
  display.display();

  if (LittleFS.begin(true)) {
    File file = LittleFS.open("/log.txt", FILE_READ);
    if (file) {
      while (file.available()) {
        String line = file.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) {
          logBuffer[logIndex] = line;
          logIndex = (logIndex + 1) % LOG_SIZE;
          if (logIndex == 0) logWrapped = true;
        }
      }
      file.close();
    }
  }

  preferences.begin("hub", false);
  stateRoom    = preferences.getUInt("state", 1);
  stateKitchen = preferences.getUInt("k", 0);
  stateNight   = preferences.getUInt("n", 0);
  statePC      = 0;
  stateTV      = 0;

  WiFi.mode(WIFI_STA);
  esp_wifi_set_mac(WIFI_IF_STA, hubMac);
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_15dBm);
  esp_wifi_set_protocol(WIFI_IF_STA,
    WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N | WIFI_PROTOCOL_LR);
  WiFi.begin(ssid, password, 11);
  configTime(10800, 0, "pool.ntp.org");

  server.on("/", handleControl);
  server.on("/api/state", handleState);
  server.on("/api/action", handleAction);
  server.on("/api/logs", handleLogs);
  server.begin();

  // При старте сразу показываем экран
  panelState = PANEL_AWAKE;
  lastActivityMs = millis();
  updateDisplay();
}

// --- LOOP ---
void loop() {
  handleButtons();
  server.handleClient();

  unsigned long now = millis();

  // Переход в сон при инактиве
  if (panelState == PANEL_AWAKE && (now - lastActivityMs >= PANEL_SLEEP_MS)) {
    panelState = PANEL_SLEEP;
    updateDisplay();
  }

  // Периодическое обновление экрана (время и т.д.), только если не спим
  if (panelState == PANEL_AWAKE && (now - lastDispRefreshMs >= DISP_REFRESH_MS)) {
    lastDispRefreshMs = now;
    updateDisplay();
  }

  if (annRoom)    { annRoom = false;    if (espNowReady) txCmd(bedroomMac,  stateRoom); }
  if (annKitchen) { annKitchen = false; if (espNowReady) txCmd(kitchenMac,  stateKitchen); }
  if (annNight)   { annNight = false;   if (espNowReady) txCmd(corridorMac, stateNight ? 0 : 1); }
  if (kitchenReported) {
    kitchenReported = false;
    stateKitchen = kitchenReportedState ? 1 : 0;
    preferences.putUInt("k", stateKitchen);
    updateDisplay();
  }

  static bool wasConnected = false;
  int wifiStatus = WiFi.status();

  if (wifiStatus != WL_CONNECTED) {
    if (wasConnected) { wasConnected = false; espNowReady = false; }
    if (now - lastHeartbeatTime >= 10000) {
      lastHeartbeatTime = now;
      WiFi.disconnect();
      WiFi.begin(ssid, password, 11);
      networkFailures++;
      if (networkFailures > 30) ESP.restart();
    }
  } else {
    if (!wasConnected) {
      wasConnected = true;
      networkFailures = 0;
      sendLog("SYS: Wi-Fi Restored. IP: " + WiFi.localIP().toString());
    }
    if (!espNowReady) {
      if (esp_now_init() == ESP_OK) {
        esp_now_register_send_cb(onDataSent);
        esp_now_register_recv_cb(onDataRecv);
        esp_now_peer_info_t peerInfo = {};
        peerInfo.channel = 11;
        peerInfo.encrypt = false;

        memcpy(peerInfo.peer_addr, bedroomMac, 6);  esp_now_add_peer(&peerInfo);
        memcpy(peerInfo.peer_addr, kitchenMac, 6);  esp_now_add_peer(&peerInfo);
        memcpy(peerInfo.peer_addr, corridorMac, 6); esp_now_add_peer(&peerInfo);

        espNowReady = true;
      }
    }
  }

  delay(5);
}
