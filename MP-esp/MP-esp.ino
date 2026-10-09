#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ArduinoOTA.h>
#include <esp_task_wdt.h>
#include "config.h"

// ── Таймауты и лимиты ────────────────────────────────────────────────────────

#define WDT_TIMEOUT_SEC          60      // аппаратный сторож: перезагрузка если loop завис
#define STREAM_SILENCE_MS        100000  // ntfy шлёт keepalive каждые 45 с; тишина дольше — соединение мертво
#define NTFY_DEAD_RESTART_MS     600000  // 10 мин без связи с ntfy — перезагрузка
#define WIFI_DEAD_RESTART_MS     180000  // 3 мин без WiFi — перезагрузка
#define WIFI_RETRY_MS            20000   // как часто дёргать WiFi.begin() при потере связи
#define DAILY_RESTART_MS         (24UL * 3600 * 1000)  // профилактическая перезагрузка раз в сутки
#define MIN_FREE_BLOCK           40000   // меньше — куча фрагментирована, TLS не влезет
#define CMD_MAX_AGE_SEC          600     // команды старше 10 мин не исполняем
#define WOL_TIMEOUT_MS           120000  // сколько ждать включения ПК после magic packet
#define LINE_MAX                 4096
#define OUTBOX_SIZE              8
#define SEEN_IDS                 32

uint8_t   targetMac[] = TARGET_MAC;
IPAddress targetIP    = IPAddress(TARGET_IP);

// ── Состояние, переживающее программную перезагрузку (WDT, ESP.restart) ─────
// После перезагрузки подписка продолжается с того же места, а уже
// исполненные сообщения не исполняются повторно.

#define RTC_MAGIC 0x57304C32

enum RestartReason : uint32_t { RR_NONE = 0, RR_DAILY, RR_WIFI, RR_NTFY, RR_HEAP, RR_OTA };

RTC_NOINIT_ATTR uint32_t rtcMagic;
RTC_NOINIT_ATTR uint32_t rtcLastTime;                // время сервера ntfy последнего события
RTC_NOINIT_ATTR char     rtcSeenIds[SEEN_IDS][16];   // id уже обработанных сообщений
RTC_NOINIT_ATTR uint32_t rtcSeenPos;
RTC_NOINIT_ATTR uint32_t rtcRestartReason;

void rtcInit() {
  esp_reset_reason_t r = esp_reset_reason();
  bool keep = rtcMagic == RTC_MAGIC && r != ESP_RST_POWERON && r != ESP_RST_BROWNOUT;
  if (!keep) {
    memset(rtcSeenIds, 0, sizeof(rtcSeenIds));
    rtcSeenPos       = 0;
    rtcLastTime      = 0;
    rtcRestartReason = RR_NONE;
    rtcMagic         = RTC_MAGIC;
  }
  if (rtcSeenPos >= SEEN_IDS) rtcSeenPos = 0;
}

bool alreadySeen(const char* id) {
  for (int i = 0; i < SEEN_IDS; i++)
    if (strncmp(rtcSeenIds[i], id, 15) == 0) return true;
  return false;
}

void markSeen(const char* id) {
  strncpy(rtcSeenIds[rtcSeenPos], id, 15);
  rtcSeenIds[rtcSeenPos][15] = '\0';
  rtcSeenPos = (rtcSeenPos + 1) % SEEN_IDS;
}

void restartNow(RestartReason reason) {
  Serial.printf("[sys] restart, reason %u\n", reason);
  rtcRestartReason = reason;
  delay(200);
  ESP.restart();
}

// ── Исходящие сообщения (очередь, не блокирует приём команд) ────────────────

String        outbox[OUTBOX_SIZE];
int           outHead = 0, outCount = 0, outTries = 0;
unsigned long outNextTryMs = 0;

void ntfyPublish(const String& msg) {
  Serial.println("[out] " + msg);
  if (outCount == OUTBOX_SIZE) {   // переполнено — выкидываем самое старое
    outHead = (outHead + 1) % OUTBOX_SIZE;
    outCount--;
    outTries = 0;
  }
  outbox[(outHead + outCount) % OUTBOX_SIZE] = msg;
  outCount++;
}

bool httpPost(const String& msg) {
  WiFiClientSecure sc;
  sc.setInsecure();
  sc.setHandshakeTimeout(10);
  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(10000);
  http.setReuse(false);
  if (!http.begin(sc, String("https://ntfy.sh/") + NTFY_TOPIC)) return false;
  http.addHeader("Content-Type", "text/plain; charset=utf-8");
  http.addHeader("Tags", "robot");   // пометка "это бот" — такие сообщения не считаются командами
  int code = http.POST(msg);
  http.end();
  if (code != 200) Serial.printf("[out] HTTP %d\n", code);
  return code == 200;
}

void flushOutbox() {
  if (outCount == 0 || millis() < outNextTryMs || WiFi.status() != WL_CONNECTED) return;
  esp_task_wdt_reset();
  if (httpPost(outbox[outHead]) || ++outTries >= 5) {
    outbox[outHead] = String();
    outHead = (outHead + 1) % OUTBOX_SIZE;
    outCount--;
    outTries = 0;
    outNextTryMs = millis() + 300;
  } else {
    outNextTryMs = millis() + 5000UL * outTries;
  }
}

// ── Статус ПК ────────────────────────────────────────────────────────────────

bool          lastPCOnline  = false;
int           statusStreak  = 0;   // сколько проверок подряд результат отличается от lastPCOnline
unsigned long lastStatusMs  = 0;

bool isPCOnline() {
  WiFiClient client;
  bool ok = client.connect(targetIP, 8765, 1000);
  client.stop();
  return ok;
}

void checkAndReportStatus() {
  bool online = isPCOnline();
  if (online == lastPCOnline) { statusStreak = 0; return; }
  // Две проверки подряд — чтобы один потерянный пакет не давал ложное "выключился"
  if (++statusStreak < 2) return;
  statusStreak = 0;
  lastPCOnline = online;
  ntfyPublish(online ? "Компьютер включился" : "Компьютер выключился / ушёл в сон");
}

// ── WoL (неблокирующий) ──────────────────────────────────────────────────────

bool          wolActive = false;
unsigned long wolStartMs = 0, wolNextCheckMs = 0;
int           wolBurst = 0;
const unsigned long WOL_BURST_AT_MS[] = { 0, 3000, 10000, 30000, 60000 };

void sendMagicPacket() {
  uint8_t packet[102];
  memset(packet, 0xFF, 6);
  for (int i = 1; i <= 16; i++)
    memcpy(&packet[i * 6], targetMac, 6);

  IPAddress subnetBcast = WiFi.localIP();
  IPAddress mask = WiFi.subnetMask();
  for (int i = 0; i < 4; i++) subnetBcast[i] |= ~mask[i];

  WiFiUDP udp;
  // UDP по WiFi теряется — шлём несколько копий на оба broadcast-адреса и оба порта
  for (int rep = 0; rep < 3; rep++) {
    for (IPAddress dst : { IPAddress(255, 255, 255, 255), subnetBcast }) {
      for (uint16_t port : { 9, 7 }) {
        udp.beginPacket(dst, port);
        udp.write(packet, sizeof(packet));
        udp.endPacket();
      }
    }
    delay(20);
  }
  Serial.println("[WoL] Sent");
}

void startWol() {
  wolActive      = true;
  wolStartMs     = millis();
  wolNextCheckMs = millis() + 5000;
  wolBurst       = 0;
  ntfyPublish("Magic packet отправлен, жду включения...");
}

void wolTick() {
  if (!wolActive) return;
  unsigned long el = millis() - wolStartMs;

  if (wolBurst < (int)(sizeof(WOL_BURST_AT_MS) / sizeof(WOL_BURST_AT_MS[0])) && el >= WOL_BURST_AT_MS[wolBurst]) {
    sendMagicPacket();
    wolBurst++;
  }

  if (millis() >= wolNextCheckMs) {
    wolNextCheckMs = millis() + 5000;
    if (isPCOnline()) {
      wolActive = false;
      lastPCOnline = true;
      statusStreak = 0;
      ntfyPublish("Компьютер включился");
      return;
    }
  }

  if (el > WOL_TIMEOUT_MS) {
    wolActive = false;
    ntfyPublish("Не удалось включить — ПК не ответил за 2 мин. Можно попробовать ещё раз");
  }
}

// ── Команды ──────────────────────────────────────────────────────────────────

// Такой же список синонимов — в pc_agent.py (COMMAND_ALIASES), менять вместе
struct Alias { const char* stem; const char* cmd; };
const Alias ALIASES[] = {
  { "выкл", "Выключить" }, { "выруб", "Выключить" }, { "off", "Выключить" }, { "shutdown", "Выключить" },
  { "вкл", "Включить" }, { "вруб", "Включить" }, { "on", "Включить" }, { "wake", "Включить" }, { "разбуд", "Включить" },
  { "сон", "Сон" }, { "спат", "Сон" }, { "усып", "Сон" }, { "sleep", "Сон" }, { "suspend", "Сон" },
  { "перезагр", "Перезагрузка" }, { "перезапус", "Перезагрузка" }, { "ребут", "Перезагрузка" },
  { "reboot", "Перезагрузка" }, { "restart", "Перезагрузка" },
  { "заблок", "Заблокировать" }, { "блок", "Заблокировать" }, { "lock", "Заблокировать" },
  { "статус", "Статус" }, { "status", "Статус" }, { "инфо", "Статус" }, { "info", "Статус" },
  { "скрин", "Скрин" }, { "снимок", "Скрин" }, { "screen", "Скрин" },
  { "помощ", "Помощь" }, { "help", "Помощь" }, { "команд", "Помощь" }, { "?", "Помощь" }, { "start", "Помощь" },
};

const char* HELP_TEXT =
  "Команды: Включить, Выключить, Сон, Перезагрузка, Заблокировать, Статус, Скрин.\n"
  "Можно коротко: вкл, выкл, ребут, блок, инфо, скрин";

// Нижний регистр для ASCII и кириллицы в UTF-8, ё → е
String lowerRu(const String& s) {
  String out;
  out.reserve(s.length());
  for (unsigned i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (c == 0xD0 && i + 1 < s.length()) {
      uint8_t n = s[i + 1];
      i++;
      if (n >= 0x90 && n <= 0x9F)      { out += (char)0xD0; out += (char)(n + 0x20); }  // А-П
      else if (n >= 0xA0 && n <= 0xAF) { out += (char)0xD1; out += (char)(n - 0x20); }  // Р-Я
      else if (n == 0x81)              { out += (char)0xD0; out += (char)0xB5; }        // Ё
      else                             { out += (char)0xD0; out += (char)n; }
    } else if (c == 0xD1 && i + 1 < s.length() && (uint8_t)s[i + 1] == 0x91) {
      out += (char)0xD0; out += (char)0xB5;                                              // ё
      i++;
    } else {
      out += (char)tolower(c);
    }
  }
  return out;
}

// "вкл", "Включи!", "/reboot" → каноническая команда; "" — не распознано
String canonicalCommand(const String& raw) {
  String s = lowerRu(raw);
  s.trim();
  while (s.startsWith("/")) s.remove(0, 1);
  for (const Alias& a : ALIASES)
    if (s.startsWith(a.stem)) return a.cmd;
  return "";
}

void processCommand(const String& text, uint32_t msgTime, uint32_t serverNow) {
  Serial.println("[cmd] " + text);

  // Страховка от петли, если тег robot вдруг не дошёл: на свои же подсказки не отвечаем
  if (text.startsWith("Не понял") || text.startsWith("Команды:")) return;

  String cmd = canonicalCommand(text);
  if (cmd == "") {
    String shown = text.length() > 40 ? text.substring(0, 40) + "…" : text;
    ntfyPublish("Не понял \"" + shown + "\".\n" + HELP_TEXT);
    return;
  }
  if (cmd == "Помощь") {
    ntfyPublish(HELP_TEXT);
    return;
  }

  if (serverNow > 0 && msgTime > 0 && serverNow > msgTime + CMD_MAX_AGE_SEC) {
    ntfyPublish("Команда \"" + cmd + "\" устарела (" + String((serverNow - msgTime) / 60) +
                " мин), пропущена. Отправь ещё раз, если актуально");
    return;
  }

  if (cmd == "Включить") {
    if (wolActive) {
      ntfyPublish("Уже включаю, жду ответа от ПК...");
    } else if (isPCOnline()) {
      lastPCOnline = true;
      ntfyPublish("Компьютер уже включён");
    } else {
      startWol();
    }
  } else {
    if (isPCOnline()) ntfyPublish("Команда принята: " + cmd);
    else              ntfyPublish("Компьютер выключен, команда \"" + cmd + "\" не выполнится");
  }
}

// ── Подписка на ntfy (одно постоянное соединение вместо опроса) ─────────────
// Опрос раз в 2 с упирался в лимит ntfy.sh (~1 запрос / 5 с на IP, вместе с
// агентом на ПК) — отсюда ответы через минуты. Поток: 1 соединение, мгновенно.

enum StreamState { SS_IDLE, SS_STATUS, SS_HEADERS, SS_CHUNK_SIZE, SS_CHUNK_DATA, SS_CHUNK_END, SS_RAW };

WiFiClientSecure stream;
StreamState   ss = SS_IDLE;
bool          chunked = false;
long          chunkLeft = 0;
char          lineBuf[LINE_MAX];
int           lineLen = 0;
bool          lineOverflow = false;
String        hdrLine;
int           httpStatus = 0;

uint32_t      serverNow = 0;          // время сервера из события open/keepalive
unsigned long lastRxMs = 0;           // последний байт из потока
unsigned long lastNtfyOkMs = 0;       // последнее живое событие от ntfy
unsigned long nextConnectMs = 0;
int           connectFails = 0;

void streamClose(unsigned long retryInMs) {
  stream.stop();
  ss = SS_IDLE;
  nextConnectMs = millis() + retryInMs;
}

void handleJsonLine(const char* line) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;

  const char* event = doc["event"] | "";
  uint32_t    t     = doc["time"] | 0;

  lastNtfyOkMs = millis();
  connectFails = 0;

  if (strcmp(event, "open") == 0 || strcmp(event, "keepalive") == 0) {
    if (t > serverNow) serverNow = t;
    if (t > rtcLastTime) rtcLastTime = t;
    return;
  }
  if (strcmp(event, "message") != 0) return;

  const char* id = doc["id"] | "";
  if (*id && alreadySeen(id)) {
    Serial.printf("[ntfy] duplicate %s skipped\n", id);
    return;
  }
  if (*id) markSeen(id);
  if (t > rtcLastTime) rtcLastTime = t;

  // Сообщения ботов (ESP, агент на ПК, скрипты) помечены тегом robot — это не команды
  for (JsonVariant tag : doc["tags"].as<JsonArray>())
    if (tag == "robot") return;

  String text = doc["message"] | "";
  text.trim();
  if (text.length() == 0) return;
  processCommand(text, t, serverNow);
}

void feedLineByte(char c) {
  if (c == '\n') {
    if (!lineOverflow && lineLen > 0) {
      lineBuf[lineLen] = '\0';
      handleJsonLine(lineBuf);
    }
    lineLen = 0;
    lineOverflow = false;
  } else if (lineLen < LINE_MAX - 1) {
    lineBuf[lineLen++] = c;
  } else {
    lineOverflow = true;
  }
}

bool streamConnect() {
  if (ESP.getMaxAllocHeap() < MIN_FREE_BLOCK) {
    Serial.printf("[ntfy] low heap block %u\n", ESP.getMaxAllocHeap());
    if (!wolActive) restartNow(RR_HEAP);
  }

  stream.stop();
  stream.setInsecure();
  stream.setHandshakeTimeout(10);
  esp_task_wdt_reset();

  Serial.println("[ntfy] connecting...");
  if (!stream.connect("ntfy.sh", 443, 10000)) {
    Serial.println("[ntfy] connect failed");
    return false;
  }

  // since: продолжаем с последнего события (дубли отсекаются по id),
  // после холодного старта — сообщения за последние 2 минуты
  String since = rtcLastTime > 0 ? String(rtcLastTime) : String("2m");
  stream.print(String("GET /") + NTFY_TOPIC + "/json?since=" + since + " HTTP/1.1\r\n"
               "Host: ntfy.sh\r\n"
               "User-Agent: esp32-wol\r\n"
               "Accept: application/x-ndjson\r\n"
               "Connection: keep-alive\r\n\r\n");

  ss = SS_STATUS;
  chunked = false;
  hdrLine = "";
  httpStatus = 0;
  lineLen = 0;
  lineOverflow = false;
  lastRxMs = millis();
  return true;
}

void streamPump() {
  int budget = 4096;
  while (budget-- > 0 && stream.available()) {
    char c = (char)stream.read();
    lastRxMs = millis();

    switch (ss) {
      case SS_STATUS:
      case SS_HEADERS:
        if (c == '\r') break;
        if (c != '\n') { if (hdrLine.length() < 256) hdrLine += c; break; }
        if (ss == SS_STATUS) {
          // "HTTP/1.1 200 OK"
          int sp = hdrLine.indexOf(' ');
          httpStatus = sp > 0 ? hdrLine.substring(sp + 1).toInt() : 0;
          ss = SS_HEADERS;
        } else if (hdrLine.length() == 0) {
          if (httpStatus != 200) {
            Serial.printf("[ntfy] HTTP %d\n", httpStatus);
            // 429 — лимит запросов, ждём подольше
            streamClose(httpStatus == 429 ? 60000 : 15000);
            return;
          }
          Serial.println("[ntfy] subscribed");
          ss = chunked ? SS_CHUNK_SIZE : SS_RAW;
        } else {
          String h = hdrLine;
          h.toLowerCase();
          if (h.startsWith("transfer-encoding:") && h.indexOf("chunked") > 0) chunked = true;
        }
        hdrLine = "";
        break;

      case SS_CHUNK_SIZE:
        if (c == '\r') break;
        if (c != '\n') { if (hdrLine.length() < 16) hdrLine += c; break; }
        chunkLeft = strtol(hdrLine.c_str(), nullptr, 16);
        hdrLine = "";
        if (chunkLeft <= 0) { streamClose(2000); return; }   // конец ответа
        ss = SS_CHUNK_DATA;
        break;

      case SS_CHUNK_DATA:
        feedLineByte(c);
        if (--chunkLeft == 0) ss = SS_CHUNK_END;
        break;

      case SS_CHUNK_END:
        if (c == '\n') ss = SS_CHUNK_SIZE;
        break;

      case SS_RAW:
        feedLineByte(c);
        break;

      default:
        break;
    }
  }
}

void ntfyTick() {
  if (ss == SS_IDLE) {
    if (millis() < nextConnectMs) return;
    if (streamConnect()) return;
    connectFails++;
    // 5, 10, 20, 40, 60, 60... секунд между попытками
    unsigned long backoff = min(60000UL, 5000UL << min(connectFails - 1, 4));
    streamClose(backoff);
    return;
  }

  streamPump();
  if (ss == SS_IDLE) return;

  if (!stream.connected() && !stream.available()) {
    Serial.println("[ntfy] connection closed");
    streamClose(2000);
  } else if (millis() - lastRxMs > STREAM_SILENCE_MS) {
    Serial.println("[ntfy] silence timeout, reconnecting");
    streamClose(1000);
  }
}

// ── WiFi ─────────────────────────────────────────────────────────────────────

unsigned long wifiLostMs = 0, wifiRetryMs = 0;
bool          otaStarted = false;

void wifiTick() {
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiLostMs) Serial.println("[WiFi] back: " + WiFi.localIP().toString());
    wifiLostMs = 0;
    if (!otaStarted) {
      ArduinoOTA.begin();
      otaStarted = true;
    }
    return;
  }

  if (!wifiLostMs) {
    wifiLostMs = millis();
    wifiRetryMs = millis();
    Serial.println("[WiFi] lost");
    if (ss != SS_IDLE) streamClose(0);
  }
  if (millis() - wifiRetryMs > WIFI_RETRY_MS) {
    wifiRetryMs = millis();
    Serial.println("[WiFi] reconnecting...");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }
  if (millis() - wifiLostMs > WIFI_DEAD_RESTART_MS) restartNow(RR_WIFI);
}

// ── OTA: прошивка по сети с ПК (когда ПК включён), без физического доступа ──

void setupOta() {
  ArduinoOTA.setHostname("wol-esp");
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() {
    Serial.println("[OTA] start");
    streamClose(60000);
    rtcRestartReason = RR_OTA;
  });
  ArduinoOTA.onProgress([](unsigned int, unsigned int) { esp_task_wdt_reset(); });
  ArduinoOTA.onError([](ota_error_t e) {
    Serial.printf("[OTA] error %u\n", e);
    rtcRestartReason = RR_NONE;
  });
}

// ── Main ─────────────────────────────────────────────────────────────────────

const char* resetReasonText() {
  switch (rtcRestartReason) {
    case RR_WIFI: return "долго не было WiFi";
    case RR_NTFY: return "долго не было связи с ntfy";
    case RR_HEAP: return "мало памяти";
    case RR_OTA:  return "обновление прошивки";
    default: break;
  }
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "включение питания";
    case ESP_RST_BROWNOUT: return "просадка питания (проверь блок питания/кабель)";
    case ESP_RST_TASK_WDT:
    case ESP_RST_INT_WDT:
    case ESP_RST_WDT:      return "зависание (сработал watchdog)";
    case ESP_RST_PANIC:    return "сбой прошивки (panic)";
    default:               return "прочее";
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);
  rtcInit();

  esp_task_wdt_config_t wdt_cfg = {
    .timeout_ms     = WDT_TIMEOUT_SEC * 1000,
    .idle_core_mask = 0,
    .trigger_panic  = true,
  };
  if (esp_task_wdt_reconfigure(&wdt_cfg) != ESP_OK) esp_task_wdt_init(&wdt_cfg);
  esp_task_wdt_add(NULL);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // без modem-sleep: быстрее отклик и стабильнее TLS-поток
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  setupOta();

  Serial.print("[WiFi] connecting");
  for (int i = 0; i < 60 && WiFi.status() != WL_CONNECTED; i++) {
    esp_task_wdt_reset();
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  // Не дождались — не страшно, wifiTick() продолжит попытки и при необходимости перезагрузит

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WiFi] " + WiFi.localIP().toString());
    lastPCOnline = isPCOnline();
  }
  lastNtfyOkMs = millis();

  if (rtcRestartReason != RR_DAILY)
    ntfyPublish(String("ESP32 запущена (") + resetReasonText() + "), ПК " +
                (lastPCOnline ? "включён" : "выключен"));
  rtcRestartReason = RR_NONE;
}

void loop() {
  esp_task_wdt_reset();

  wifiTick();

  if (WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.handle();
    ntfyTick();
    wolTick();
    flushOutbox();

    if (!wolActive && millis() - lastStatusMs > STATUS_CHECK_INTERVAL_MS) {
      checkAndReportStatus();
      lastStatusMs = millis();
    }
  }

  if (millis() - lastNtfyOkMs > NTFY_DEAD_RESTART_MS && !wolActive) restartNow(RR_NTFY);

  // Профилактика: раз в сутки, когда ничего не происходит
  if (millis() > DAILY_RESTART_MS && !wolActive && outCount == 0) restartNow(RR_DAILY);

  delay(10);
}
