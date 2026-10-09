#include "secrets.h"
#include <WiFi.h>
#include <WebServer.h>
#include "esp_camera.h"
#include "FS.h"
#include "SD_MMC.h"

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASS;

#define PIR_PIN 14 // Пин, к которому подключен OUT датчика AM312

WebServer server(80);

// Глобальные переменные состояния
uint32_t latestPhotoId = 0;
String latestPhotoPath = "";
bool lastPirState = false;

const char INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Охранная камера</title>
  <style>
    body { font-family: sans-serif; text-align: center; margin: 20px; background: #121212; color: #fff; }
    #status { margin-bottom: 20px; font-size: 18px; color: #4caf50; }
    #status.alert { color: #f44336; font-weight: bold; }
    img { max-width: 100%; max-height: 80vh; border-radius: 8px; display: none; box-shadow: 0 4px 12px rgba(0,0,0,0.5); }
  </style>
</head>
<body>
  <h2>Мониторинг движения</h2>
  <div id="status">Ожидание...</div>
  <img id="photo" src="">

  <script>
    let currentId = "0";
    const statusDiv = document.getElementById('status');
    const photoImg = document.getElementById('photo');

    setInterval(async () => {
      try {
        let res = await fetch('/status');
        let newId = await res.text();
        
        if (newId !== currentId && newId !== "0") {
          currentId = newId;
          photoImg.src = '/latest.jpg?t=' + new Date().getTime();
          photoImg.style.display = 'inline-block';
          
          statusDiv.innerText = 'ДВИЖЕНИЕ ОБНАРУЖЕНО!';
          statusDiv.className = 'alert';
          
          setTimeout(() => {
            statusDiv.innerText = 'Ожидание...';
            statusDiv.className = '';
          }, 3000);
        }
      } catch(e) {
        console.error("Ошибка связи");
      }
    }, 2000); // Опрос сервера каждые 2 секунды
  </script>
</body>
</html>
)rawliteral";

// Поиск последнего индекса файла при загрузке
int getNextFileIndex() {
  int index = 0;
  while (SD_MMC.exists("/motion_" + String(index) + ".jpg")) {
    index++;
  }
  return index;
}

// Захват и сохранение
void captureAndSave() {
  camera_fb_t * fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Сбой камеры!");
    return;
  }

  String path = "/motion_" + String(getNextFileIndex()) + ".jpg";
  File file = SD_MMC.open(path, FILE_WRITE);
  
  if (!file) {
    Serial.println("Ошибка записи на SD!");
  } else {
    file.write(fb->buf, fb->len);
    file.close();
    Serial.println("Сохранено: " + path);
    
    // Обновляем глобальные переменные для веб-сервера
    latestPhotoPath = path;
    latestPhotoId = millis(); 
  }
  
  esp_camera_fb_return(fb);
}

// Маршруты сервера
void handleRoot() {
  server.send(200, "text/html", INDEX_HTML);
}

void handleStatus() {
  server.send(200, "text/plain", String(latestPhotoId));
}

void handleLatest() {
  if (latestPhotoPath == "" || !SD_MMC.exists(latestPhotoPath)) {
    server.send(404, "text/plain", "Файл не найден");
    return;
  }
  File file = SD_MMC.open(latestPhotoPath, FILE_READ);
  server.streamFile(file, "image/jpeg");
  file.close();
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  // Настройка пина PIR
  pinMode(PIR_PIN, INPUT);

  // Инициализация SD
  SD_MMC.setPins(39, 38, 40);
  if (!SD_MMC.begin("/sdcard", true)) {
    Serial.println("Ошибка монтирования SD");
    return;
  }

  // Настройка камеры
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = 11;
  config.pin_d1 = 9;
  config.pin_d2 = 8;
  config.pin_d3 = 10;
  config.pin_d4 = 12;
  config.pin_d5 = 18;
  config.pin_d6 = 17;
  config.pin_d7 = 16;
  config.pin_xclk = 15;
  config.pin_pclk = 13;
  config.pin_vsync = 6;
  config.pin_href = 7;
  config.pin_sccb_sda = 4;
  config.pin_sccb_scl = 5;
  config.pin_pwdn = -1;
  config.pin_reset = -1;
  
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  
  // Для фото возвращаем максимальное качество!
  config.frame_size = FRAMESIZE_UXGA; 
  config.jpeg_quality = 10;
  config.fb_count = 2;
  config.grab_mode = CAMERA_GRAB_LATEST;

  if (psramFound()) {
    config.fb_location = CAMERA_FB_IN_PSRAM;
  } else {
    config.fb_location = CAMERA_FB_IN_DRAM;
    config.fb_count = 1;
  }

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("Камера не инициализирована");
    return;
  }

  // Wi-Fi
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  Serial.println(WiFi.localIP());

  // Маршруты
  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/latest.jpg", handleLatest);
  server.begin();
}

void loop() {
  server.handleClient();

  // Чтение состояния датчика
  bool currentPirState = digitalRead(PIR_PIN);
  
  // Реагируем только на передний фронт (переход из LOW в HIGH)
  if (currentPirState && !lastPirState) {
    Serial.println("Движение! Делаю фото...");
    captureAndSave();
  }
  
  lastPirState = currentPirState;
}
