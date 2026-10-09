#include "secrets.h"
/*  Автополив света для кошачьей травы (овёс/пшеница/ячмень)
    Плата: ESP32-S3-WROOM-1 N16R8 CAM (распиновка Freenove), OV2640, SD сзади (SDMMC 1-bit)
    Лента: DLED 2835 12V 6500K -> через MOSFET IRLB8721 (low-side, gate=GPIO2 через 220R, pulldown 10k)
    Питание: БП 12В -> лента напрямую; LM2596 12В->5В -> пин 5V платы. GND общий.

    Возможности:
      - "рассвет/закат" по реальному времени (NTP), плавный fade с гамма-коррекцией
      - таймлапс раз в час на SD (/timelapse/*.jpg)
      - анализ роста (доля зелени) + рекомендации на локальном сайте http://<ip>/

    Среда: Arduino IDE, плата "ESP32S3 Dev Module", PSRAM: OPI, Partition: 16M Flash.
    Зависимости: ядро esp32 by Espressif 3.x (других библиотек не нужно).
*/

#include "esp_camera.h"
#include "img_converters.h"
#include <WiFi.h>
#include <WebServer.h>
#include <SD_MMC.h>
#include "time.h"

// ----------------------- НАСТРОЙКИ -----------------------

// Часовой пояс (Краснодар = UTC+3, без перехода на лето)
const long  GMT_OFFSET_SEC = 3 * 3600;
const int   DST_OFFSET_SEC = 0;
const char* NTP_SERVER     = "pool.ntp.org";

// Световой режим (минуты от полуночи)
const int SUNRISE_START = 7 * 60;    // начало рассвета 07:00
const int SUNSET_START  = 21 * 60;   // начало заката   21:00
const int FADE_MIN      = 30;        // длительность плавного перехода, мин
const float MAX_BRIGHT  = 1.00f;     // макс. яркость 0..1

// Свет / ШИМ
const int   LED_PIN   = 2;
const int   PWM_FREQ  = 8000;
const int   PWM_RES   = 12;          // 12 бит -> 4095 уровней
const int   PWM_MAXD  = (1 << PWM_RES) - 1;

// Камера/таймлапс/анализ
#define FRAME_SZ      FRAMESIZE_SVGA // 800x600 (баланс качество/память)
const int  PX_STEP    = 8;           // шаг прореживания пикселей при анализе
const int  GRAPH_N    = 48;          // точек истории для графика (часов)

// ----------------------- ПИНЫ КАМЕРЫ (Freenove S3) -----------------------
#define PWDN_GPIO_NUM  -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM  15
#define SIOD_GPIO_NUM  4
#define SIOC_GPIO_NUM  5
#define Y9_GPIO_NUM    16
#define Y8_GPIO_NUM    17
#define Y7_GPIO_NUM    18
#define Y6_GPIO_NUM    12
#define Y5_GPIO_NUM    10
#define Y4_GPIO_NUM    8
#define Y3_GPIO_NUM    9
#define Y2_GPIO_NUM    11
#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM  7
#define PCLK_GPIO_NUM  13
// SD (SDMMC 1-bit): CLK=39, CMD=38, D0=40

// ----------------------- ГЛОБАЛЬНОЕ СОСТОЯНИЕ -----------------------
WebServer server(80);

float curBrightPct = 0;          // текущая яркость, %
bool  lightOn      = false;
float greenHist[GRAPH_N];        // история доли зелени, %
int   histCount    = 0;
float lastGreen    = 0, lastBright = 0;
int   lastCapHour  = -1;
time_t startEpoch  = 0;          // момент запуска выращивания
String statusText  = "Ожидание данных...";
String recoText    = "";

// ----------------------- СВЕТ -----------------------
// Яркость 0..1 по расписанию суток
float scheduleLevel(int minutes) {
  if (minutes < SUNRISE_START || minutes >= SUNSET_START + FADE_MIN) return 0.0f; // ночь
  if (minutes < SUNRISE_START + FADE_MIN)                                          // рассвет
    return (float)(minutes - SUNRISE_START) / FADE_MIN;
  if (minutes >= SUNSET_START)                                                     // закат
    return 1.0f - (float)(minutes - SUNSET_START) / FADE_MIN;
  return 1.0f;                                                                     // день
}

void applyLight() {
  struct tm t;
  if (!getLocalTime(&t)) return;
  float lvl = scheduleLevel(t.tm_hour * 60 + t.tm_min) * MAX_BRIGHT;
  curBrightPct = lvl * 100.0f;
  lightOn = lvl > 0.001f;
  float duty = powf(lvl, 2.2f) * PWM_MAXD;        // гамма-коррекция для плавности
  ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, (uint32_t)(duty + 0.5f));
  ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
}

// ----------------------- АНАЛИЗ РОСТА -----------------------
void analyzeFrame(camera_fb_t* fb) {
  uint8_t* rgb = (uint8_t*)ps_malloc((size_t)fb->width * fb->height * 3);
  if (!rgb) return;
  if (fmt2rgb888(fb->buf, fb->len, fb->format, rgb)) {
    long total = 0, green = 0, sumBright = 0;
    size_t n = (size_t)fb->width * fb->height * 3;
    for (size_t i = 0; i + 2 < n; i += 3 * PX_STEP) {
      uint8_t r = rgb[i], g = rgb[i + 1], b = rgb[i + 2]; // зелёный = средний канал
      total++;
      sumBright += (r + g + b) / 3;
      if (g > 60 && g > r * 1.15f && g > b * 1.10f) green++;
    }
    if (total > 0) {
      lastGreen  = 100.0f * green / total;
      lastBright = (float)sumBright / total; // 0..255
    }
  }
  free(rgb);
}

void pushHistory(float v) {
  if (histCount < GRAPH_N) greenHist[histCount++] = v;
  else { for (int i = 1; i < GRAPH_N; i++) greenHist[i - 1] = greenHist[i]; greenHist[GRAPH_N - 1] = v; }
}

// Движок статуса/рекомендаций
void evaluate() {
  int days = startEpoch ? (int)((time(nullptr) - startEpoch) / 86400L) : 0;

  // тренд: среднее за последние ~6 ч против пика
  float peak = 0; for (int i = 0; i < histCount; i++) peak = max(peak, greenHist[i]);
  float recent = 0; int rc = 0;
  for (int i = max(0, histCount - 6); i < histCount; i++) { recent += greenHist[i]; rc++; }
  if (rc) recent /= rc;

  recoText = "";
  if (days <= 3 && lastGreen < 3) {
    statusText = "Прорастание (день " + String(days) + ")";
    recoText = "Норма для первых дней. Держите субстрат влажным, 18-22°C, без прямого пересыхания.";
  } else if (days >= 4 && lastGreen < 3) {
    statusText = "Внимание: всходов нет";
    recoText = "Проверьте: влажность субстрата, свежесть семян, температуру 18-22°C. Возможно, семена не взошли.";
  } else if (peak > 8 && lastGreen < peak * 0.7f) {
    statusText = "Внимание: зелень убывает";
    recoText = "Пересыхание, перелив или плесень. Проверьте полив и проветривание, уберите загнившие участки.";
  } else if (lastBright < 40 && lightOn) {
    statusText = "Внимание: темно в кадре";
    recoText = "Низкая освещённость — проверьте ленту/MOSFET/контакты питания.";
  } else if (lastGreen >= 40 && days >= 8) {
    statusText = "Трава готова";
    recoText = "Можно давать коту или подстричь на 2-3 см для нового роста.";
  } else if (recent <= peak * 0.95f && histCount > 12 && days > 5) {
    statusText = "Рост замедлен";
    recoText = "Возможна нехватка света или питания. Можно увеличить фотопериод/добавить слабую подкормку.";
  } else {
    statusText = "Рост в норме (день " + String(days) + ", зелень " + String(lastGreen, 1) + "%)";
    recoText = "Всё хорошо, продолжайте обычный уход.";
  }
}

// ----------------------- ТАЙМЛАПС + ЦИКЛ АНАЛИЗА -----------------------
void capture() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { Serial.println("Ошибка захвата кадра"); return; }

  // 1) таймлапс на SD
  struct tm t; getLocalTime(&t);
  char path[48];
  snprintf(path, sizeof(path), "/timelapse/%04d%02d%02d_%02d%02d.jpg",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min);
  File f = SD_MMC.open(path, FILE_WRITE);
  if (f) { f.write(fb->buf, fb->len); f.close(); Serial.printf("Сохранено: %s\n", path); }

  // 2) анализ роста
  analyzeFrame(fb);
  esp_camera_fb_return(fb);

  pushHistory(lastGreen);
  evaluate();

  // 3) лог в CSV
  File c = SD_MMC.open("/growth.csv", FILE_APPEND);
  if (c) { c.printf("%ld,%.2f,%.1f\n", (long)time(nullptr), lastGreen, lastBright); c.close(); }
}

// ----------------------- ВЕБ-САЙТ -----------------------
const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang=ru><meta charset=utf-8>
<meta name=viewport content="width=device-width,initial-scale=1">
<title>Кошачья трава</title>
<style>
 body{font-family:system-ui,sans-serif;margin:0;background:#0f1411;color:#e8efe9}
 .wrap{max-width:760px;margin:0 auto;padding:16px}
 h1{font-size:20px;margin:8px 0 16px}
 .card{background:#1a221d;border:1px solid #2a352e;border-radius:12px;padding:14px;margin-bottom:14px}
 .row{display:flex;gap:14px;flex-wrap:wrap}
 .row .card{flex:1;min-width:140px}
 .big{font-size:28px;font-weight:700;color:#7ed957}
 .lbl{font-size:12px;color:#8aa094;text-transform:uppercase;letter-spacing:.5px}
 img{width:100%;border-radius:10px;display:block}
 .warn{color:#ffcf5c}.ok{color:#7ed957}
 svg{width:100%;height:120px}
</style>
<div class=wrap>
 <h1>🌱 Освещение кошачьей травы</h1>
 <div class=row>
  <div class=card><div class=lbl>Свет</div><div class=big id=br>—</div></div>
  <div class=card><div class=lbl>Зелень</div><div class=big id=gr>—</div></div>
  <div class=card><div class=lbl>Время</div><div class=big id=tm>—</div></div>
 </div>
 <div class=card><div class=lbl>Статус</div><div id=st style=font-size:18px;margin-top:4px>—</div>
  <div id=rc style=margin-top:8px;color:#b9c8be></div></div>
 <div class=card><div class=lbl>История зелени, %</div><svg id=ch viewBox="0 0 480 120" preserveAspectRatio=none></svg></div>
 <div class=card><div class=lbl>Камера</div><img id=cam src=/snapshot></div>
</div>
<script>
async function upd(){
 let d=await (await fetch('/data')).json();
 br.textContent=d.bright.toFixed(0)+'%'; gr.textContent=d.green.toFixed(1)+'%'; tm.textContent=d.time;
 st.textContent=d.status; st.className=d.status.includes('Внимание')?'warn':'ok';
 rc.textContent=d.reco;
 let h=d.hist,mx=Math.max(5,...h),w=480,n=h.length,p='';
 h.forEach((v,i)=>{let x=n>1?i/(n-1)*w:0,y=120-v/mx*110-5;p+=(i?'L':'M')+x.toFixed(1)+' '+y.toFixed(1)+' '});
 ch.innerHTML='<polyline fill=none stroke=#7ed957 stroke-width=2 points="'+
   h.map((v,i)=>{let x=n>1?i/(n-1)*w:0,y=120-v/mx*110-5;return x.toFixed(1)+','+y.toFixed(1)}).join(' ')+'"/>';
 cam.src='/snapshot?'+Date.now();
}
upd();setInterval(upd,30000);
</script></html>)HTML";

void handleRoot() { server.send_P(200, "text/html", PAGE); }

void handleSnapshot() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { server.send(503, "text/plain", "no frame"); return; }
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "image/jpeg", (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
}

void handleData() {
  struct tm t; char tb[8] = "--:--";
  if (getLocalTime(&t)) snprintf(tb, sizeof(tb), "%02d:%02d", t.tm_hour, t.tm_min);
  String h = "[";
  for (int i = 0; i < histCount; i++) { h += String(greenHist[i], 1); if (i < histCount - 1) h += ","; }
  h += "]";
  String j = "{\"bright\":" + String(curBrightPct, 1) +
             ",\"green\":" + String(lastGreen, 1) +
             ",\"time\":\"" + tb + "\"" +
             ",\"status\":\"" + statusText + "\"" +
             ",\"reco\":\"" + recoText + "\"" +
             ",\"hist\":" + h + "}";
  server.send(200, "application/json", j);
}

// ----------------------- ИНИЦИАЛИЗАЦИЯ -----------------------
bool initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0; c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0=Y2_GPIO_NUM;c.pin_d1=Y3_GPIO_NUM;c.pin_d2=Y4_GPIO_NUM;c.pin_d3=Y5_GPIO_NUM;
  c.pin_d4=Y6_GPIO_NUM;c.pin_d5=Y7_GPIO_NUM;c.pin_d6=Y8_GPIO_NUM;c.pin_d7=Y9_GPIO_NUM;
  c.pin_xclk=XCLK_GPIO_NUM;c.pin_pclk=PCLK_GPIO_NUM;c.pin_vsync=VSYNC_GPIO_NUM;c.pin_href=HREF_GPIO_NUM;
  c.pin_sccb_sda=SIOD_GPIO_NUM;c.pin_sccb_scl=SIOC_GPIO_NUM;
  c.pin_pwdn=PWDN_GPIO_NUM;c.pin_reset=RESET_GPIO_NUM;
  c.xclk_freq_hz=20000000;c.frame_size=FRAME_SZ;c.pixel_format=PIXFORMAT_JPEG;
  c.grab_mode=CAMERA_GRAB_LATEST;c.fb_location=CAMERA_FB_IN_PSRAM;c.jpeg_quality=12;c.fb_count=2;
  return esp_camera_init(&c) == ESP_OK;
}

void setup() {
  Serial.begin(115200);

  // Свет: таймер 1 + канал 2, чтобы не конфликтовать с камерой (timer0/channel0)
  ledc_timer_config_t lt = {};
  lt.speed_mode      = LEDC_LOW_SPEED_MODE;
  lt.timer_num       = LEDC_TIMER_1;
  lt.duty_resolution = (ledc_timer_bit_t)PWM_RES;
  lt.freq_hz         = PWM_FREQ;
  lt.clk_cfg         = LEDC_AUTO_CLK;
  ledc_timer_config(&lt);

  ledc_channel_config_t lc = {};
  lc.speed_mode = LEDC_LOW_SPEED_MODE;
  lc.channel    = LEDC_CHANNEL_2;
  lc.timer_sel  = LEDC_TIMER_1;
  lc.intr_type  = LEDC_INTR_DISABLE;
  lc.gpio_num   = LED_PIN;
  lc.duty       = 0;
  lc.hpoint     = 0;
  ledc_channel_config(&lc);

  if (!initCamera()) Serial.println("Камера не инициализирована!");

  // SD (1-bit, слот сзади)
  SD_MMC.setPins(39, 38, 40);
  if (!SD_MMC.begin("/sdcard", true)) Serial.println("SD не найдена!");
  else if (!SD_MMC.exists("/timelapse")) SD_MMC.mkdir("/timelapse");

  // WiFi + время
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) { delay(250); Serial.print('.'); }
  Serial.printf("\nIP: %s\n", WiFi.localIP().toString().c_str());
  configTime(GMT_OFFSET_SEC, DST_OFFSET_SEC, NTP_SERVER);

  // момент старта выращивания (из файла или сейчас)
  File s = SD_MMC.open("/start.txt", FILE_READ);
  if (s) { startEpoch = s.parseInt(); s.close(); }
  if (startEpoch < 1000000000) {
    struct tm tmp; int wait = 0;
    while (!getLocalTime(&tmp) && wait++ < 20) delay(500);
    startEpoch = time(nullptr);
    File w = SD_MMC.open("/start.txt", FILE_WRITE);
    if (w) { w.print((long)startEpoch); w.close(); }
  }

  server.on("/", handleRoot);
  server.on("/snapshot", handleSnapshot);
  server.on("/data", handleData);
  server.on("/favicon.ico", [](){ server.send(204); });
  server.begin();

  capture();                           // стартовый кадр
}

void loop() {
  server.handleClient();

  static unsigned long tLight = 0, tCheck = 0;
  unsigned long now = millis();

  if (now - tLight > 1000) { tLight = now; applyLight(); }

  if (now - tCheck > 10000) {           // проверка часа раз в 10 сек
    tCheck = now;
    struct tm t;
    if (getLocalTime(&t) && t.tm_min == 0 && t.tm_hour != lastCapHour) {
      lastCapHour = t.tm_hour;
      capture();
    }
  }
}
