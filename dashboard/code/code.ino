#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <time.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

const int BTN_NEXT = 4;
const int BTN_PREV = 5;

// Настройки Wi-Fi
const char* ssid = "";
const char* password = "";

// Замени IP на тот, что выдал роутер твоему ПК с сервером (например, 192.168.31.100)
const char* pcApiUrl = "http://192.168.31.31:5000/api/pc"; 

const long  gmtOffset_sec = 3 * 3600;
const int   daylightOffset_sec = 0;
const char* ntpServer = "pool.ntp.org";

int currentScreen = 0;
const int MAX_SCREENS = 5; // 0: SysInfo, 1: Time, 2: PC Stats

unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 150;

unsigned long lastDisplayUpdate = 0;
unsigned long lastPcUpdate = 0;
const unsigned long pcUpdateInterval = 2000; // Опрос ПК каждые 2 секунды

String timeData = "--:--:--";
String pcCpu = "--";
String pcGpu = "--";
String pcRam = "--/-- Gb";
String pcWeatherTemp = "--";
String pcWeatherDesc = "Loading...";
String pcMedia = "Loading...";
String pcDisk = "--";

void setup() {
  Serial.begin(115200);

  pinMode(BTN_NEXT, INPUT_PULLUP);
  pinMode(BTN_PREV, INPUT_PULLUP);

  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("SSD1306 allocation failed"));
    for(;;); 
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 20);
  display.println("Connecting WiFi...");
  display.display();

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  updateTimeData();
  updateDisplay();
}

void loop() {
  handleButtons();

  if (millis() - lastDisplayUpdate > 1000) {
    updateTimeData();
    if (currentScreen == 1) {
      updateDisplay();
    }
    lastDisplayUpdate = millis();
  }

  if (currentScreen >= 2 && (millis() - lastPcUpdate > pcUpdateInterval)) {
    fetchPcStats();
    updateDisplay();
    lastPcUpdate = millis();
  }
}

void handleButtons() {
  if ((millis() - lastDebounceTime) > debounceDelay) {
    if (digitalRead(BTN_NEXT) == LOW) {
      currentScreen = (currentScreen + 1) % MAX_SCREENS;
      updateDisplay();
      lastDebounceTime = millis();
    }
    if (digitalRead(BTN_PREV) == LOW) {
      currentScreen = (currentScreen - 1 + MAX_SCREENS) % MAX_SCREENS;
      updateDisplay();
      lastDebounceTime = millis();
    }
  }
}

void updateTimeData() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;
  
  char timeStringBuff[10];
  strftime(timeStringBuff, sizeof(timeStringBuff), "%H:%M:%S", &timeinfo);
  timeData = String(timeStringBuff);
}

void fetchPcStats() {
  if (WiFi.status() == WL_CONNECTED) {
    WiFiClient client;
    HTTPClient http;
    http.begin(client, pcApiUrl);
    
    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      JsonDocument doc;
      
      if (!deserializeJson(doc, payload)) {
        pcCpu = String((int)doc["cpu"]);
        pcGpu = String((int)doc["gpu"]);
        pcWeatherTemp = doc["weather_temp"].as<String>();
        pcWeatherDesc = doc["weather_desc"].as<String>();
        pcMedia = doc["media"].as<String>();
        pcDisk = String((int)doc["disk_free"]);
        
        char ramBuf[16];
        snprintf(ramBuf, sizeof(ramBuf), "%d/%d Gb", (int)doc["ram_used"], (int)doc["ram_total"]);
        pcRam = String(ramBuf);
      }
    }
    http.end();
  }
}

void updateDisplay() {
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  switch (currentScreen) {
    case 0:
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.println("--- SYSTEM INFO ---");
      display.setCursor(0, 20);
      display.print("WiFi: ");
      display.println(WiFi.SSID());
      display.setCursor(0, 35);
      display.print("IP: ");
      display.println(WiFi.localIP());
      break;

    case 1:
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.println("--- MSK TIME ---");
      display.setTextSize(2); 
      display.setCursor(15, 30);
      display.println(timeData);
      break;
      
    case 2:
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.println("--- PC STATS ---");
      display.setCursor(0, 20);
      display.print("CPU Load: ");
      display.print(pcCpu);
      display.println(" %");
      display.setCursor(0, 35);
      display.print("GPU Load: ");
      display.print(pcGpu);
      display.println(" %");
      display.setCursor(0, 50);
      display.print("RAM Use:  ");
      display.println(pcRam);
      break;

    case 3:
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.println("--- KRASNODAR ---");
      display.setTextSize(3);
      display.setCursor(0, 18);
      display.println(pcWeatherTemp);
      display.setTextSize(1);
      display.setCursor(0, 48);
      display.println(pcWeatherDesc);
      break;

    case 4:
      display.setTextSize(1);
      display.setCursor(0, 0);
      display.println("--- FEDORA ---");
      display.setCursor(0, 20);
      display.print("Free Space: ");
      display.print(pcDisk);
      display.println(" Gb");
      display.setCursor(0, 35);
      display.println("Now Playing:");
      display.setCursor(0, 45);
      display.println(pcMedia);
      break;
  }
  
  display.display();
}
