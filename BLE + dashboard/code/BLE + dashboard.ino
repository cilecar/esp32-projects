#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <SPI.h>
#include <SD.h>
#include <BleKeyboard.h>
#include <esp_sleep.h>
#include <WiFi.h>
#include <time.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "mbedtls/aes.h"
#include "mbedtls/sha256.h"

// Пины
#define PIN_ENC_S1 25
#define PIN_ENC_S2 26
#define PIN_ENC_BTN 27
#define PIN_SD_CS 4

// Настройки Wi-Fi и API
const char* ssid = "";
const char* password = "";
const char* pcApiUrl = "http://192.168.31.31:5000/api/pc"; 
const long  gmtOffset_sec = 3 * 3600;
const int   daylightOffset_sec = 0;
const char* ntpServer = "pool.ntp.org";

// Настройки сна
#define SLEEP_TIMEOUT_MS 300000 // 5 минут бездействия -> глубокий сон
unsigned long lastActivityTime = 0;

// Глобальные объекты
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, 22, 21);
BleKeyboard bleKeyboard("BLE Vault", "ESP32", 100);

// Состояния системы
enum AppState {
    STATE_DASHBOARD,
    STATE_PIN_ENTRY,
    STATE_ERROR,
    STATE_VAULT
};
AppState currentState = STATE_DASHBOARD;

// Переменные энкодера
volatile int encoderPos = 0;
volatile int lastEncoded = 0;
volatile int rawTicks = 0;
int currentSelection = 0;
unsigned long lastButtonPress = 0;
const unsigned long debounceDelay = 200;
bool redrawRequired = true;

// Переменные Дашборда
const int MAX_SCREENS = 6; // 0..4 (Дашборды), 5 (Вход в Vault)
unsigned long lastDisplayUpdate = 0;
unsigned long lastPcUpdate = 0;
const unsigned long pcUpdateInterval = 2000;
String timeData = "--:--:--";
String pcCpu = "--";
String pcGpu = "--";
String pcRam = "--/-- Gb";
String pcWeatherTemp = "--";
String pcWeatherDesc = "Loading...";
String pcMedia = "Loading...";
String pcDisk = "--";

// Переменные Vault
const uint8_t MAX_RECORDS = 20;
String serviceNames[MAX_RECORDS];
String passwords[MAX_RECORDS];
uint8_t totalRecords = 0;
int pinDigits[4] = {0, 0, 0, 0};
int currentPinIndex = 0;
String errorMessage = "";

void IRAM_ATTR encoderISR() {
    int MSB = digitalRead(PIN_ENC_S1);
    int LSB = digitalRead(PIN_ENC_S2);
    int encoded = (MSB << 1) | LSB;
    int sum = (lastEncoded << 2) | encoded;

    if (sum == 0b1101 || sum == 0b0100 || sum == 0b0010 || sum == 0b1011) rawTicks++;
    if (sum == 0b1110 || sum == 0b0111 || sum == 0b0001 || sum == 0b1000) rawTicks--;

    lastEncoded = encoded;

    // 1 физический щелчок = 4 фазы. Меняем позицию только при полном щелчке!
    if (rawTicks >= 4) {
        encoderPos++;
        rawTicks = 0;
    } else if (rawTicks <= -4) {
        encoderPos--;
        rawTicks = 0;
    }
}

void goToDeepSleep() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x13_t_cyrillic);
    u8g2.drawUTF8(30, 35, "Спящий режим...");
    u8g2.sendBuffer();
    delay(1000);
    
    u8g2.setPowerSave(1);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_27, 0);
    esp_deep_sleep_start();
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

bool decryptVault() {
    if (!SD.begin(PIN_SD_CS)) {
        errorMessage = "Ошибка SD-карты";
        return false;
    }

    File file = SD.open("/vault.enc", FILE_READ);
    if (!file) {
        errorMessage = "Файл не найден";
        return false;
    }

    size_t fileSize = file.size();
    if (fileSize <= 16 || fileSize % 16 != 0) {
        errorMessage = "Неверный формат";
        file.close();
        return false;
    }

    String pinStr = String(pinDigits[0]) + String(pinDigits[1]) + String(pinDigits[2]) + String(pinDigits[3]);
    uint8_t key[32];
    mbedtls_sha256_ret((const unsigned char*)pinStr.c_str(), 4, key, 0);

    uint8_t iv[16];
    file.read(iv, 16);

    size_t dataSize = fileSize - 16;
    uint8_t* encData = new uint8_t[dataSize];
    file.read(encData, dataSize);
    file.close();

    uint8_t* decData = new uint8_t[dataSize];
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, key, 256);
    mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, dataSize, iv, encData, decData);
    mbedtls_aes_free(&aes);

    uint8_t padLen = decData[dataSize - 1];
    if (padLen == 0 || padLen > 16) {
        errorMessage = "Неверный ПИН-код";
        delete[] encData;
        delete[] decData;
        return false;
    }

    size_t validDataSize = dataSize - padLen;
    String plainText = "";
    for (size_t i = 0; i < validDataSize; i++) {
        plainText += (char)decData[i];
    }
    delete[] encData;
    delete[] decData;

    totalRecords = 0;
    int startIndex = 0;
    while (startIndex < plainText.length() && totalRecords < MAX_RECORDS) {
        int newlineIndex = plainText.indexOf('\n', startIndex);
        if (newlineIndex == -1) newlineIndex = plainText.length();

        String line = plainText.substring(startIndex, newlineIndex);
        line.trim();

        if (line.length() > 0) {
            int separatorIdx = line.indexOf(':');
            if (separatorIdx > 0) {
                serviceNames[totalRecords] = line.substring(0, separatorIdx);
                passwords[totalRecords] = line.substring(separatorIdx + 1);
                totalRecords++;
            }
        }
        startIndex = newlineIndex + 1;
    }

    if (totalRecords == 0) {
        errorMessage = "Пустой файл";
        return false;
    }
    return true;
}

void setup() {
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) delay(500);

    pinMode(PIN_ENC_S1, INPUT_PULLUP);
    pinMode(PIN_ENC_S2, INPUT_PULLUP);
    pinMode(PIN_ENC_BTN, INPUT_PULLUP);

    attachInterrupt(digitalPinToInterrupt(PIN_ENC_S1), encoderISR, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_S2), encoderISR, CHANGE);

    u8g2.begin();
    u8g2.enableUTF8Print();
    u8g2.setPowerSave(0);

    // Подключение к Wi-Fi (с таймаутом, чтобы не зависнуть)
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x13_t_cyrillic);
    u8g2.drawUTF8(10, 30, "Поиск Wi-Fi...");
    u8g2.sendBuffer();

    WiFi.begin(ssid, password);
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 15) {
        delay(500);
        retries++;
    }

    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    updateTimeData();
    
    bleKeyboard.begin();
    lastActivityTime = millis();
}

void drawUI() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x13_t_cyrillic);

    if (currentState == STATE_DASHBOARD) {
        switch (currentSelection) {
            case 0:
                u8g2.drawUTF8(0, 10, "--- СИСТЕМА ---");
                u8g2.setCursor(0, 30); u8g2.print("WiFi: "); u8g2.print(WiFi.SSID());
                u8g2.setCursor(0, 45); u8g2.print("IP: "); u8g2.print(WiFi.localIP());
                break;
            case 1:
                u8g2.drawUTF8(0, 10, "--- ВРЕМЯ МСК ---");
                u8g2.setFont(u8g2_font_logisoso16_tf);
                u8g2.setCursor(15, 45); u8g2.print(timeData);
                break;
            case 2:
                u8g2.drawUTF8(0, 10, "--- СТАТУС ПК ---");
                u8g2.setCursor(0, 25); u8g2.print("CPU: "); u8g2.print(pcCpu); u8g2.print(" %");
                u8g2.setCursor(0, 40); u8g2.print("GPU: "); u8g2.print(pcGpu); u8g2.print(" %");
                u8g2.setCursor(0, 55); u8g2.print("RAM: "); u8g2.print(pcRam);
                break;
            case 3:
                u8g2.drawUTF8(0, 10, "--- КРАСНОДАР ---");
                u8g2.setFont(u8g2_font_logisoso16_tf);
                u8g2.setCursor(0, 35); u8g2.print(pcWeatherTemp);
                u8g2.setFont(u8g2_font_6x13_t_cyrillic);
                u8g2.setCursor(0, 55); u8g2.print(pcWeatherDesc);
                break;
            case 4:
                u8g2.drawUTF8(0, 10, "--- МЕДИА ---");
                u8g2.setCursor(0, 25); u8g2.print("Свободно: "); u8g2.print(pcDisk); u8g2.print(" Gb");
                u8g2.setCursor(0, 40); u8g2.print("Трек:");
                u8g2.setCursor(0, 55); u8g2.print(pcMedia);
                break;
            case 5:
                u8g2.drawUTF8(0, 10, "--- БАЗА ПАРОЛЕЙ ---");
                u8g2.drawBox(5, 30, 118, 16);
                u8g2.setDrawColor(0);
                u8g2.drawUTF8(15, 42, "Ввести ПИН-код");
                u8g2.setDrawColor(1);
                break;
        }
    } else if (currentState == STATE_PIN_ENTRY) {
        u8g2.drawUTF8(20, 15, "Введите ПИН-код");
        for (int i = 0; i < 4; i++) {
            int x = 30 + (i * 18);
            if (i == currentPinIndex) {
                u8g2.drawBox(x - 2, 25, 12, 16);
                u8g2.setDrawColor(0);
            } else {
                u8g2.setDrawColor(1);
            }
            u8g2.setCursor(x, 38);
            u8g2.print(pinDigits[i]);
            u8g2.setDrawColor(1);
        }
    } else if (currentState == STATE_ERROR) {
        u8g2.drawUTF8(10, 35, errorMessage.c_str());
        u8g2.drawUTF8(10, 55, "Клик для возврата");
    } else if (currentState == STATE_VAULT) {
        if (!bleKeyboard.isConnected()) u8g2.drawUTF8(0, 10, "Ожидание BLE...");
        else u8g2.drawUTF8(0, 10, "BLE Подключен");
        u8g2.drawLine(0, 13, 128, 13);

        int startIdx = currentSelection > 2 ? currentSelection - 2 : 0;
        for (int i = 0; i < 3; i++) {
            int itemIdx = startIdx + i;
            if (itemIdx <= totalRecords) { // <= чтобы включить кнопку "Выход"
                int yPos = 28 + (i * 15);
                if (itemIdx == currentSelection) {
                    u8g2.drawBox(0, yPos - 11, 128, 14);
                    u8g2.setDrawColor(0);
                } else u8g2.setDrawColor(1);
                
                u8g2.setCursor(5, yPos);
                if (itemIdx == totalRecords) u8g2.print("<- Назад в меню");
                else u8g2.print(serviceNames[itemIdx]);
                u8g2.setDrawColor(1);
            }
        }
    }
    u8g2.sendBuffer();
}

void loop() {
    if (millis() - lastActivityTime > SLEEP_TIMEOUT_MS) goToDeepSleep();

    // Фоновое обновление дашбордов
    if (currentState == STATE_DASHBOARD) {
        if (millis() - lastDisplayUpdate > 1000) {
            updateTimeData();
            if (currentSelection == 1) redrawRequired = true;
            lastDisplayUpdate = millis();
        }
        if (currentSelection >= 2 && currentSelection <= 4 && (millis() - lastPcUpdate > pcUpdateInterval)) {
            fetchPcStats();
            redrawRequired = true;
            lastPcUpdate = millis();
        }
    }

    int newPos = encoderPos; // БОЛЬШЕ НИКАКОГО ДЕЛЕНИЯ НА 4!

    if (currentState == STATE_DASHBOARD) {
        if (newPos != currentSelection) {
            lastActivityTime = millis();
            if (newPos >= MAX_SCREENS) { newPos = 0; encoderPos = 0; } 
            else if (newPos < 0) { newPos = MAX_SCREENS - 1; encoderPos = MAX_SCREENS - 1; }
            currentSelection = newPos;
            redrawRequired = true;
        }

        if (digitalRead(PIN_ENC_BTN) == LOW) {
            unsigned long currentTime = millis();
            if (currentTime - lastButtonPress > debounceDelay) {
                lastActivityTime = currentTime;
                if (currentSelection == 5) {
                    currentState = STATE_PIN_ENTRY;
                    currentPinIndex = 0;
                    encoderPos = 0;
                    for (int i = 0; i < 4; i++) pinDigits[i] = 0;
                }
                redrawRequired = true;
                lastButtonPress = currentTime;
            }
        }
    } else if (currentState == STATE_PIN_ENTRY) {
        if (newPos != pinDigits[currentPinIndex]) {
            lastActivityTime = millis();
            if (newPos > 9) { newPos = 0; encoderPos = 0; } 
            else if (newPos < 0) { newPos = 9; encoderPos = 9; }
            pinDigits[currentPinIndex] = newPos;
            redrawRequired = true;
        }

        if (digitalRead(PIN_ENC_BTN) == LOW) {
            unsigned long currentTime = millis();
            if (currentTime - lastButtonPress > debounceDelay) {
                lastActivityTime = currentTime;
                currentPinIndex++;
                if (currentPinIndex > 3) {
                    u8g2.clearBuffer();
                    u8g2.drawUTF8(20, 35, "Расшифровка...");
                    u8g2.sendBuffer();
                    
                    if (decryptVault()) {
                        currentState = STATE_VAULT;
                        btStart(); // <--- ВКЛЮЧАЕМ BLUETOOTH ПРИ УСПЕХЕ
                        encoderPos = 0;
                        currentSelection = 0;
                    } else {
                        currentState = STATE_ERROR;
                    }
                } else {
                    encoderPos = pinDigits[currentPinIndex];
                }
                redrawRequired = true;
                lastButtonPress = currentTime;
            }
        }
    } else if (currentState == STATE_ERROR) {
        if (digitalRead(PIN_ENC_BTN) == LOW) {
            unsigned long currentTime = millis();
            if (currentTime - lastButtonPress > debounceDelay) {
                lastActivityTime = currentTime;
                currentState = STATE_DASHBOARD;
                currentSelection = 5;
                encoderPos = 5;
                redrawRequired = true;
                lastButtonPress = currentTime;
            }
        }
    } else if (currentState == STATE_VAULT) {
        if (newPos != currentSelection) {
            lastActivityTime = millis();
            if (newPos > totalRecords) { newPos = 0; encoderPos = 0; } 
            else if (newPos < 0) { newPos = totalRecords; encoderPos = totalRecords; }
            currentSelection = newPos;
            redrawRequired = true;
        }

        if (digitalRead(PIN_ENC_BTN) == LOW) {
            unsigned long currentTime = millis();
            if (currentTime - lastButtonPress > debounceDelay) {
                lastActivityTime = currentTime;
                if (currentSelection == totalRecords) {
                    // Кнопка "Назад"
                    currentState = STATE_DASHBOARD;
                    btStop(); // <--- ВЫКЛЮЧАЕМ BLUETOOTH ПРИ ВЫХОДЕ
                    currentSelection = 5;
                    encoderPos = 5;
                } else if (bleKeyboard.isConnected() && passwords[currentSelection].length() > 0) {
                    bleKeyboard.print(passwords[currentSelection]);
                    u8g2.clearBuffer();
                    u8g2.drawUTF8(10, 35, "Пароль отправлен!");
                    u8g2.sendBuffer();
                    delay(1000);
                }
                redrawRequired = true;
                lastButtonPress = currentTime;
            }
        }
    }

    if (redrawRequired) {
        drawUI();
        redrawRequired = false;
    }
    delay(10);
}
