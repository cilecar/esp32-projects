#include "secrets.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASS;
const String tgProxyUrl = String(SERVER_URL) + "/esp-alert";
const String tgSyncUrl = String(SERVER_URL) + "/esp-sync";

const uint8_t trigPin = 5;
const uint8_t echoPin = 18;
const uint8_t alertLedPin = 2;
const uint8_t buttonPin = 4;
const uint8_t pirPin = 27;

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

volatile bool isArmed = false;
volatile bool localStateChanged = false; // Флаг локального нажатия
bool ledState = false;

volatile float currentDistance = 0.0;
volatile float alarmDistanceToReport = 0.0; 
volatile uint8_t alarmTypeToReport = 0;

unsigned long previousSensorMillis = 0;
const unsigned long sensorInterval = 50;
unsigned long previousLedMillis = 0;
const unsigned long ledBlinkInterval = 150;
unsigned long previousTelegramMillis = 0;
const unsigned long telegramCooldown = 5000;

bool lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
const unsigned long debounceDelay = 50;

const uint8_t NUM_SAMPLES = 5;
float samples[NUM_SAMPLES] = {0};
uint8_t sampleIndex = 0;

TaskHandle_t networkTaskHandle = NULL;

// Единая сетевая задача (Тревоги + Синхронизация)
void networkTask(void *pvParameters) {
  WiFiClientSecure client;
  client.setInsecure(); 
  HTTPClient http;

  for (;;) {
    // Ждем сигнал тревоги до 3 секунд. Если его нет - идем на синхронизацию статуса
    uint32_t notificationValue = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000));

    if (WiFi.status() == WL_CONNECTED) {
      
      // 1. Отправка тревоги
      if (notificationValue > 0) {
        String url = tgProxyUrl;
        if (alarmTypeToReport == 2) {
          url += "?event=ultrasonic&distance=" + String(alarmDistanceToReport, 1);
        } else {
          url += "?event=motion";
        }
        http.begin(client, url);
        http.GET();
        http.end();
      }
      
      // 2. Синхронизация статуса охраны
      String syncUrl = tgSyncUrl;
      bool sendingLocalState = localStateChanged;
      
      if (sendingLocalState) {
        syncUrl += "?set_state=" + String(isArmed ? "1" : "0");
        localStateChanged = false;
      }
      
      http.begin(client, syncUrl);
      int httpCode = http.GET();
      
      if (httpCode > 0) {
        String payload = http.getString();
        payload.trim();
        
        // Если мы не отправляли локальное изменение, значит применяем статус из Telegram
        if (!sendingLocalState) {
          bool serverArmed = (payload == "1");
          if (serverArmed != isArmed) {
            isArmed = serverArmed;
            if (!isArmed) digitalWrite(alertLedPin, LOW);
          }
        }
      }
      http.end();
    }
  }
}

void sortArray(float* array, uint8_t size) {
  for (uint8_t i = 0; i < size - 1; i++) {
    for (uint8_t j = 0; j < size - i - 1; j++) {
      if (array[j] > array[j + 1]) {
        float temp = array[j];
        array[j] = array[j + 1];
        array[j + 1] = temp;
      }
    }
  }
}

float getMedianDistance() {
  float sortedSamples[NUM_SAMPLES];
  memcpy(sortedSamples, samples, sizeof(samples));
  sortArray(sortedSamples, NUM_SAMPLES);
  return sortedSamples[NUM_SAMPLES / 2];
}

void setup() {
  Serial.begin(115200);

  pinMode(trigPin, OUTPUT);
  pinMode(echoPin, INPUT);
  pinMode(alertLedPin, OUTPUT);
  pinMode(buttonPin, INPUT_PULLUP);
  pinMode(pirPin, INPUT);
  digitalWrite(alertLedPin, LOW);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    for(;;);
  }
  
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 20);
  display.println("Connecting...");
  display.display();

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }

  xTaskCreatePinnedToCore(
    networkTask,
    "NetworkTask",
    8192,
    NULL,
    1,
    &networkTaskHandle,
    0
  );
}

void loop() {
  unsigned long currentMillis = millis();

  // Локальная кнопка
  bool reading = digitalRead(buttonPin);
  if (reading != lastButtonState) {
    lastDebounceTime = currentMillis;
  }
  if ((currentMillis - lastDebounceTime) > debounceDelay) {
    static bool buttonState = HIGH;
    if (reading != buttonState) {
      buttonState = reading;
      if (buttonState == LOW) {
        isArmed = !isArmed;
        localStateChanged = true; // Даем сигнал серверу обновить кнопку в чате
        if (!isArmed) digitalWrite(alertLedPin, LOW); 
      }
    }
  }
  lastButtonState = reading;

  // Опрос дальномера
  if (currentMillis - previousSensorMillis >= sensorInterval) {
    previousSensorMillis = currentMillis;

    digitalWrite(trigPin, LOW);
    delayMicroseconds(2);
    digitalWrite(trigPin, HIGH);
    delayMicroseconds(10);
    digitalWrite(trigPin, LOW);

    long duration = pulseIn(echoPin, HIGH, 25000);
    float rawDistance = (duration == 0) ? 400.0 : (duration * 0.0343 / 2.0);

    samples[sampleIndex] = rawDistance;
    sampleIndex = (sampleIndex + 1) % NUM_SAMPLES;
    currentDistance = getMedianDistance();
  }

  bool isPirActive = digitalRead(pirPin);
  // Лимит дальномера изменен на 100 см
  bool isUltrasonicActive = (currentDistance > 0 && currentDistance < 100.0);

  if (isArmed) {
    if (isUltrasonicActive || isPirActive) {
      
      if (currentMillis - previousLedMillis >= ledBlinkInterval) {
        previousLedMillis = currentMillis;
        ledState = !ledState;
        digitalWrite(alertLedPin, ledState);
      }

      if (currentMillis - previousTelegramMillis >= telegramCooldown) {
        previousTelegramMillis = currentMillis;
        
        if (isUltrasonicActive) {
          alarmTypeToReport = 2;
          alarmDistanceToReport = currentDistance;
        } else {
          alarmTypeToReport = 1;
        }
        
        xTaskNotifyGive(networkTaskHandle);
      }
      
    } else {
      digitalWrite(alertLedPin, LOW);
    }
  } else {
    digitalWrite(alertLedPin, LOW);
  }

  // Отрисовка дисплея
  display.clearDisplay();
  
  display.setTextSize(1);
  display.setCursor(0, 0);
  if (isArmed) {
    display.print("Status: ARMED ");
    if (isPirActive) display.print("[MOT]");
  } else {
    display.print("Status: OFF");
  }

  display.setTextSize(2);
  display.setCursor(0, 30);
  if (currentDistance >= 400 || currentDistance <= 2) {
    display.println("Out of range");
  } else {
    display.print(currentDistance, 1);
    display.println(" cm");
  }
  
  display.display();
}
