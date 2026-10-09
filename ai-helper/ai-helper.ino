#include "secrets.h"
/*
 * ESP32-S3 (Freenove WROOM N8R8) + OV2640 + SSD1306 128x64 I2C
 *
 * Кнопка "снять" -> кадр -> прокси (proxy.py) -> Gemini -> текст на дисплей.
 * Кнопки "вверх"/"вниз" листают длинный ответ.
 *
 * Настройки Arduino IDE (проверено на живой плате):
 *   Board            : ESP32S3 Dev Module
 *   PSRAM            : OPI PSRAM          <-- обязательно, иначе камера не встанет
 *   Flash Size       : 16MB (128Mb)       <-- на этой плате именно 16 МБ
 *   Partition Scheme : Huge APP (3MB No OTA/1MB SPIFFS)
 *   USB CDC On Boot  : Disabled           <-- см. ниже
 *   ESP32 core       : 3.x  (в 2.x поля структуры камеры звались pin_sscb_*)
 *
 * Про USB CDC On Boot. На плате два USB-разъёма: "UART" через мост CH343
 * (1a86:55d3) и нативный USB самого ESP32-S3 (GPIO19/20). Кабель в UART-порту =>
 * Serial должен идти в UART0 (GPIO43/44), для этого CDC On Boot = Disabled.
 * Если воткнётесь в нативный USB — наоборот, поставьте Enabled, иначе лог
 * уйдёт в никуда и плата будет выглядеть зависшей.
 *
 * Библиотеки: U8g2 (Oliver Kraus).
 */
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <U8g2lib.h>
#include "esp_camera.h"

// ---------------- Настройки пользователя ----------------


// IP машины, где запущен proxy.py.
static const char *PROXY_URL = "http://192.168.31.31:8000/describe";

// ---------------- Камера (распаяна на плате) ----------------
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD     4
#define CAM_PIN_SIOC     5
#define CAM_PIN_Y2      11
#define CAM_PIN_Y3       9
#define CAM_PIN_Y4       8
#define CAM_PIN_Y5      10
#define CAM_PIN_Y6      12
#define CAM_PIN_Y7      18
#define CAM_PIN_Y8      17
#define CAM_PIN_Y9      16
#define CAM_PIN_VSYNC    6
#define CAM_PIN_HREF     7
#define CAM_PIN_PCLK    13

// ---------------- Периферия ----------------
// Камера занимает 4-13 и 15-18. Ещё заняты: 35-37 (октальный PSRAM),
// 19/20 (USB), 43/44 (UART0), 48 (RGB-LED на плате), 38-40 (слот SD).
// 45/46 не берём под кнопки: это strapping-пины, подтяжка вверх ломает загрузку.
#define OLED_SDA        1
#define OLED_SCL        2
#define BTN_SHOOT_PIN  21   // все кнопки замыкают пин на GND
#define BTN_UP_PIN     14
#define BTN_DOWN_PIN   42

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);

// Шрифты с кириллицей. Если текст всё ещё мелкий — поднимите FONT_TEXT на
// u8g2_font_8x13_t_cyrillic (16 симв./строку) или 9x15 (14 симв.), и вместе с ним
// LINE_H. Доступны также 6x12, 6x13, 10x20 и unifont.
#define FONT_TEXT u8g2_font_7x13_t_cyrillic  // 18 символов в строке
#define FONT_BIG  u8g2_font_10x20_t_cyrillic // для статусов, 12 символов

static const int LINE_H        = 13;
static const int VISIBLE_LINES = 4;    // 4 * 13 = 52 из 64 px
static const int MAX_LINES     = 32;   // буфер прокрутки
static const int TEXT_W        = 124;  // 128 минус полоса прокрутки справа

// ---------------- Состояние экрана ----------------
static String textLines[MAX_LINES];
static int lineCount    = 0;
static int scrollOffset = 0;

// Дисплей опционален: пока он не подпаян, прошивка работает и отдаёт всё в Serial.
// Рисование в буфер идёт всегда (оно чисто в памяти), по I2C уходит только при hasDisplay.
static bool hasDisplay = false;

// ---------------- Стадии работы ----------------
// Сеть выполняется отдельной задачей на втором ядре, поэтому loop() остаётся
// свободным и может крутить анимацию, пока идёт запрос.
enum Phase : uint8_t {
  PH_IDLE,     // ждём нажатия
  PH_WARMUP,   // прогрев автоэкспозиции
  PH_CAPTURE,  // снятие кадра
  PH_UPLOAD,   // соединение и отправка
  PH_WAIT,     // ждём ответ Gemini
};

static volatile Phase phase   = PH_IDLE;
static volatile bool  netBusy = false;  // задача жива
static volatile bool  netDone = false;  // результат готов
static String netResult;                // пишется задачей до netDone = true
static uint32_t phaseStartedAt = 0;     // для счётчика секунд на экране

// ---------------- Кнопки ----------------
static const uint16_t DEBOUNCE_MS   = 30;
static const uint16_t REPEAT_DELAY  = 450;  // до начала автоповтора
static const uint16_t REPEAT_PERIOD = 120;  // шаг автоповтора

struct Button {
  uint8_t  pin;
  bool     prev        = HIGH;
  uint32_t changedAt   = 0;
  uint32_t nextRepeat  = 0;
};

static Button btnShoot{BTN_SHOOT_PIN};
static Button btnUp{BTN_UP_PIN};
static Button btnDown{BTN_DOWN_PIN};

// Возвращает true в момент нажатия, а при repeatMs != 0 — ещё и на автоповторе.
static bool poll(Button &b, uint16_t repeatMs = 0) {
  const bool now = digitalRead(b.pin);
  const uint32_t t = millis();

  if (now != b.prev && t - b.changedAt > DEBOUNCE_MS) {
    b.changedAt = t;
    b.prev = now;
    if (now == LOW) {
      b.nextRepeat = t + REPEAT_DELAY;
      return true;
    }
  }
  if (repeatMs && b.prev == LOW && now == LOW && t >= b.nextRepeat) {
    b.nextRepeat = t + repeatMs;
    return true;
  }
  return false;
}

// ---------------- Разбор и вывод текста ----------------

// Длина UTF-8 последовательности по её первому байту.
static uint8_t utf8Len(uint8_t c) {
  if ((c & 0x80) == 0x00) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;  // битый байт — просто пропускаем
}

static void pushLine(const String &s) {
  if (lineCount < MAX_LINES) textLines[lineCount++] = s;
}

// Слово само шире строки — режем по границам символов, а не байтов.
static void breakLongWord(String &word) {
  while (lineCount < MAX_LINES && u8g2.getUTF8Width(word.c_str()) > TEXT_W) {
    String chunk;
    size_t k = 0;
    while (k < word.length()) {
      uint8_t l = utf8Len((uint8_t)word[k]);
      String probe = chunk + word.substring(k, k + l);
      if (u8g2.getUTF8Width(probe.c_str()) > TEXT_W) break;
      chunk = probe;
      k += l;
    }
    if (chunk.isEmpty()) break;  // страховка от зацикливания
    pushLine(chunk);
    word = word.substring(k);
  }
}

static void wrapText(const String &text) {
  lineCount = 0;
  scrollOffset = 0;
  u8g2.setFont(FONT_TEXT);  // ширина строк считается по тому же шрифту, что и рисуется

  String cur, word;
  String src = text + " ";  // хвостовой пробел гарантирует сброс последнего слова
  size_t i = 0;

  while (i < src.length() && lineCount < MAX_LINES) {
    const uint8_t b = (uint8_t)src[i];

    if (b >= 0x80) {  // многобайтовый символ забираем целиком
      const uint8_t l = utf8Len(b);
      word += src.substring(i, i + l);
      i += l;
      continue;
    }

    const char c = (char)b;
    if (c != ' ' && c != '\n' && c != '\r') {
      word += c;
      i++;
      continue;
    }

    // Граница слова: пробуем дописать его в текущую строку.
    if (word.length()) {
      String probe = cur.isEmpty() ? word : cur + " " + word;
      if (u8g2.getUTF8Width(probe.c_str()) <= TEXT_W) {
        cur = probe;
      } else {
        if (cur.length()) { pushLine(cur); cur = ""; }
        breakLongWord(word);
        cur = word;
      }
      word = "";
    }
    if (c == '\n' && cur.length()) { pushLine(cur); cur = ""; }
    i++;
  }
  if (cur.length()) pushLine(cur);
}

static void render() {
  u8g2.clearBuffer();
  u8g2.setFont(FONT_TEXT);

  const int visible = min(lineCount - scrollOffset, VISIBLE_LINES);
  for (int i = 0; i < visible; i++)
    u8g2.drawUTF8(0, (i + 1) * LINE_H, textLines[scrollOffset + i].c_str());

  // Полоса прокрутки — только если текст не влез целиком.
  if (lineCount > VISIBLE_LINES) {
    const int trackH = VISIBLE_LINES * LINE_H;
    const int maxOff = lineCount - VISIBLE_LINES;
    const int barH   = max(4, trackH * VISIBLE_LINES / lineCount);
    const int barY   = (trackH - barH) * scrollOffset / maxOff;
    u8g2.drawVLine(127, 0, trackH);
    u8g2.drawBox(125, barY, 3, barH);
  }
  if (hasDisplay) u8g2.sendBuffer();
}

static void showText(const String &text) {
  wrapText(text);
  render();
}

// Простое сообщение по центру крупным шрифтом (стартовые экраны, ошибки).
static void showStatus(const char *msg) {
  Serial.printf("[статус] %s\n", msg);
  lineCount = 0;
  scrollOffset = 0;
  u8g2.clearBuffer();
  u8g2.setFont(FONT_BIG);
  int w = u8g2.getUTF8Width(msg);
  if (w > 128) {  // не влезло крупным — падаем на мелкий
    u8g2.setFont(FONT_TEXT);
    w = u8g2.getUTF8Width(msg);
  }
  u8g2.drawUTF8((128 - w) / 2, 38, msg);
  if (hasDisplay) u8g2.sendBuffer();
}

// ---------------- Анимация ожидания ----------------

// Бегущая по кругу "комета": голова яркая, хвост затухает.
static void drawSpinner(int cx, int cy, int r, uint8_t step) {
  for (int i = 0; i < 8; i++) {
    const float a = i * (2 * PI / 8);
    const int x = cx + (int)roundf(cosf(a) * r);
    const int y = cy + (int)roundf(sinf(a) * r);
    const int age = (step + 8 - i) % 8;   // 0 = голова
    if (age == 0)      u8g2.drawDisc(x, y, 2);
    else if (age == 1) u8g2.drawDisc(x, y, 1);
    else if (age == 2) u8g2.drawPixel(x, y);
  }
}

// Диафрагма, схлопывающаяся к центру, — для стадии съёмки.
static void drawShutter(int cx, int cy, uint8_t step) {
  const int r = 12 - (step % 6) * 2;
  u8g2.drawCircle(cx, cy, 12);
  if (r > 0) u8g2.drawDisc(cx, cy, r);
}

// Пакет, улетающий вверх, — для стадии отправки.
static void drawUpload(int cx, int cy, uint8_t step) {
  for (int i = 0; i < 3; i++) {
    const int y = cy + 8 - ((step + i * 2) % 6) * 3;
    u8g2.drawBox(cx - 3, y, 7, 2);
  }
  u8g2.drawTriangle(cx, cy - 12, cx - 6, cy - 5, cx + 6, cy - 5);
}

// Экран стадии: крупное слово, секундомер и анимация под ним.
static void drawBusy(Phase p, uint8_t step) {
  const char *word;
  switch (p) {
    case PH_WARMUP:  word = "Навожу";    break;
    case PH_CAPTURE: word = "Снимаю";    break;
    case PH_UPLOAD:  word = "Отправляю"; break;
    default:         word = "Думаю";     break;
  }

  u8g2.clearBuffer();
  u8g2.setFont(FONT_BIG);
  u8g2.drawUTF8((128 - u8g2.getUTF8Width(word)) / 2, 18, word);

  // Секундомер: без него 30-40 секунд ожидания выглядят как зависание.
  const uint32_t sec = (millis() - phaseStartedAt) / 1000;
  char timer[16];
  snprintf(timer, sizeof(timer), "%lu с", (unsigned long)sec);
  u8g2.setFont(FONT_TEXT);
  u8g2.drawUTF8((128 - u8g2.getUTF8Width(timer)) / 2, 62, timer);

  switch (p) {
    case PH_WARMUP:
    case PH_CAPTURE: drawShutter(64, 38, step); break;
    case PH_UPLOAD:  drawUpload(64, 38, step);  break;
    default:         drawSpinner(64, 38, 11, step % 8); break;
  }
  if (hasDisplay) u8g2.sendBuffer();
}

// Ищем SSD1306 на шине. Модули бывают и на 0x3C, и на 0x3D.
static bool detectDisplay() {
  Wire.begin(OLED_SDA, OLED_SCL);
  for (uint8_t addr : {0x3C, 0x3D}) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("Дисплей найден на 0x%02X\n", addr);
      if (addr != 0x3C) u8g2.setI2CAddress(addr << 1);  // до begin()
      return true;
    }
  }
  Serial.println("Дисплей не найден — вывод только в Serial");
  return false;
}

static void scrollBy(int delta) {
  if (lineCount <= VISIBLE_LINES) return;
  const int maxOff = lineCount - VISIBLE_LINES;
  const int next = constrain(scrollOffset + delta, 0, maxOff);
  if (next != scrollOffset) {
    scrollOffset = next;
    render();
  }
}

// ---------------- Камера ----------------
static bool initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0 = CAM_PIN_Y2;  c.pin_d1 = CAM_PIN_Y3;
  c.pin_d2 = CAM_PIN_Y4;  c.pin_d3 = CAM_PIN_Y5;
  c.pin_d4 = CAM_PIN_Y6;  c.pin_d5 = CAM_PIN_Y7;
  c.pin_d6 = CAM_PIN_Y8;  c.pin_d7 = CAM_PIN_Y9;
  c.pin_xclk     = CAM_PIN_XCLK;
  c.pin_pclk     = CAM_PIN_PCLK;
  c.pin_vsync    = CAM_PIN_VSYNC;
  c.pin_href     = CAM_PIN_HREF;
  c.pin_sccb_sda = CAM_PIN_SIOD;
  c.pin_sccb_scl = CAM_PIN_SIOC;
  c.pin_pwdn     = CAM_PIN_PWDN;
  c.pin_reset    = CAM_PIN_RESET;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size   = FRAMESIZE_VGA;       // 640x480: выше смысла мало, трафик растёт
  c.jpeg_quality = 10;                  // меньше число = лучше качество и больше размер
  c.fb_count     = 2;
  c.fb_location  = CAMERA_FB_IN_PSRAM;
  c.grab_mode    = CAMERA_GRAB_LATEST;  // всегда свежий кадр, а не лежавший в очереди

  const esp_err_t err = esp_camera_init(&c);
  if (err != ESP_OK) {
    Serial.printf("esp_camera_init: 0x%x\n", err);
    return false;
  }
  if (sensor_t *s = esp_camera_sensor_get()) {
    s->set_vflip(s, 0);
    s->set_hmirror(s, 0);

    // Помещение OV2640 стабильно недоэкспонирует. Тянем выдержку, а не усиление:
    // усиление добавляет шум, а не детали, поэтому потолок держим умеренным.
    s->set_aec2(s, 1);                       // AEC через DSP, заметно лучше в полумраке
    s->set_ae_level(s, 2);                   // цель автоэкспозиции выше (-2..2)
    s->set_gainceiling(s, GAINCEILING_16X);
    s->set_brightness(s, 1);                 // -2..2
    s->set_contrast(s, 1);                   // -2..2
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
  }
  return true;
}

// OV2640 первые кадры после простоя отдаёт тёмными/зелёными — даём автоэкспозиции
// отработать, выбрасывая их.
static void cameraWarmup(uint8_t frames) {
  for (uint8_t i = 0; i < frames; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
    delay(120);
  }
}

// ---------------- Запрос к прокси ----------------
// Выполняется в отдельной задаче, поэтому здесь можно свободно блокироваться:
// экран в это время перерисовывает loop() на другом ядре.
static String askAI(uint8_t warmupFrames) {
  const uint32_t tStart = millis();

  phase = PH_WARMUP;
  cameraWarmup(warmupFrames);
  const uint32_t tWarm = millis();

  phase = PH_CAPTURE;
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return "Ошибка камеры";
  const uint32_t tCap = millis();
  Serial.printf("Кадр: %u байт, RSSI %d dBm | прогрев %lu мс, съёмка %lu мс\n",
                (unsigned)fb->len, WiFi.RSSI(),
                (unsigned long)(tWarm - tStart), (unsigned long)(tCap - tWarm));
  phase = PH_UPLOAD;

  // Отрицательные коды HTTPClient — это проблемы связи (не достучались, оборвалась
  // передача тела). По Wi-Fi такое случается, поэтому пробуем несколько раз;
  // положительный код — это уже ответ сервера, его повторять смысла нет.
  // Бюджет таймаутов согласован с прокси: тот тратит на Gemini максимум
  // 2 попытки * 20 с + пауза, то есть ~42 с. Read timeout здесь заведомо больше,
  // иначе плата сдаётся, пока сервер ещё работает, и шлёт кадр повторно —
  // сервер начинает новый запрос к Gemini, и ожидание удваивается.
  String out;
  for (int attempt = 1; attempt <= 2; attempt++) {
    HTTPClient http;
    http.setConnectTimeout(5000);
    http.setTimeout(55000);
    if (!http.begin(PROXY_URL)) {
      out = "Не открыть URL";
      break;
    }
    http.addHeader("Content-Type", "image/jpeg");

    // POST блокируется до конца ответа: сюда входит и отправка кадра, и работа
    // Gemini на той стороне. Это самая долгая стадия, её и показываем.
    phase = PH_WAIT;
    const uint32_t tPost = millis();
    const int code = http.POST(fb->buf, fb->len);
    const uint32_t dt = millis() - tPost;

    if (code == 200) {
      out = http.getString();
      Serial.printf("  попытка %d: ответ за %lu мс\n", attempt, (unsigned long)dt);
      http.end();
      break;
    }
    if (code > 0) {
      out = "HTTP " + String(code);
      Serial.printf("  попытка %d: HTTP %d за %lu мс\n", attempt, code, (unsigned long)dt);
      http.end();
      break;
    }

    out = "Связь: " + HTTPClient::errorToString(code);
    Serial.printf("  попытка %d/2 не удалась за %lu мс: %s (%d)\n",
                  attempt, (unsigned long)dt, out.c_str(), code);
    http.end();

    // Таймаут чтения означает, что запрос ушёл и сервер, возможно, всё сделал.
    // Повторять смысла нет — только удвоим нагрузку и ожидание.
    if (code == HTTPC_ERROR_READ_TIMEOUT) break;

    phase = PH_UPLOAD;  // на повторе снова показываем стадию отправки
    delay(500);
    phase = PH_WAIT;
  }

  Serial.printf("Итого цикл: %lu мс\n", (unsigned long)(millis() - tStart));
  esp_camera_fb_return(fb);
  return out;
}

// Задача сети живёт на ядре 1, рядом с loop(). Ядро 0 нарочно оставлено стеку
// Wi-Fi: при закреплении задачи на нулевом ядре отправка кадра начинала рваться
// (send payload failed / read timeout) — они конкурировали за одно ядро.
static void netTask(void *arg) {
  const String answer = askAI(6);
  netResult = answer;   // пишем результат ДО флага готовности
  phase = PH_IDLE;
  netDone = true;
  netBusy = false;
  vTaskDelete(nullptr);
}

static void startShot() {
  if (netBusy) return;  // уже снимаем — повторные нажатия игнорируем
  netBusy = true;
  netDone = false;
  phase = PH_WARMUP;
  phaseStartedAt = millis();
  // 12 КБ стека: HTTPClient со String внутри заметно жрёт.
  xTaskCreatePinnedToCore(netTask, "net", 12288, nullptr, 1, nullptr, 1);
}

// ---------------- setup / loop ----------------
void setup() {
  Serial.begin(115200);
  delay(400);  // даём USB-CDC подняться, иначе первые строки лога теряются
  Serial.println("\n=== ai-helper ===");

  pinMode(BTN_SHOOT_PIN, INPUT_PULLUP);
  pinMode(BTN_UP_PIN,    INPUT_PULLUP);
  pinMode(BTN_DOWN_PIN,  INPUT_PULLUP);

  hasDisplay = detectDisplay();  // строго до begin(): там выбирается адрес
  u8g2.begin();
  u8g2.setFont(FONT_TEXT);       // нужен всегда — по нему считается ширина строк
  showStatus("Старт...");

  if (!psramFound()) {
    showStatus("Нет PSRAM!");  // включите OPI PSRAM в настройках платы
    while (true) delay(1000);
  }
  if (!initCamera()) {
    showStatus("Камера не заведена");
    while (true) delay(1000);
  }

  showStatus("Wi-Fi...");
  WiFi.mode(WIFI_STA);
  // По умолчанию станция засыпает между маяками точки доступа. На отправке
  // кадра это давало разброс от 1 до 15 секунд на одни и те же 20 КБ:
  // каждый TCP-обмен ждал следующего окна пробуждения, а повторы множили задержку.
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) delay(250);

  if (WiFi.status() != WL_CONNECTED) {
    showStatus("Нет Wi-Fi");
  } else {
    Serial.printf("IP: %s, RSSI %d dBm, сервер: %s\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI(), PROXY_URL);
    showText("Готов. Нажми кнопку съёмки.");
  }
  Serial.println("Съёмка: кнопка на GPIO21 либо Enter в этом терминале.");
}

void loop() {
  bool shoot = poll(btnShoot);

  // Любой байт из Serial (проще всего — Enter) равносилен нажатию кнопки.
  if (Serial.available()) {
    while (Serial.available()) Serial.read();
    shoot = true;
  }

  if (shoot && !netBusy) {
    if (WiFi.status() != WL_CONNECTED) {
      showStatus("Нет Wi-Fi");
      WiFi.reconnect();
    } else {
      startShot();
    }
  }

  // Пока работает задача сети — крутим анимацию стадии.
  if (netBusy) {
    static uint32_t lastFrame = 0;
    static uint8_t step = 0;
    if (millis() - lastFrame >= 90) {   // ~11 кадров в секунду
      lastFrame = millis();
      drawBusy(phase, step++);
    }
    delay(5);
    return;                              // во время съёмки кнопки прокрутки не нужны
  }

  // Задача завершилась — забираем результат.
  if (netDone) {
    netDone = false;
    Serial.printf("[ответ] %s\n", netResult.c_str());
    showText(netResult.length() ? netResult : "Пустой ответ");
    Serial.printf("[вёрстка] строк: %d\n", lineCount);
    // Пока шёл запрос, кнопки прокрутки могли зажать — сбрасываем их состояние.
    btnUp.prev = digitalRead(BTN_UP_PIN);
    btnDown.prev = digitalRead(BTN_DOWN_PIN);
  }

  if (poll(btnUp,   REPEAT_PERIOD)) scrollBy(-1);
  if (poll(btnDown, REPEAT_PERIOD)) scrollBy(+1);

  delay(5);
}
