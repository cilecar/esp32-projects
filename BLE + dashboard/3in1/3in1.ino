#include "secrets.h"
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
#include <vector>
#include <WiFiUdp.h>
#include <WebServer.h>

#define PIN_ENC_S1 16
#define PIN_ENC_S2 17
#define PIN_ENC_BTN 15
#define PIN_SD_CS 4
#define SLEEP_TIMEOUT_MS 300000

const char* ssid = WIFI_SSID;
const char* password = WIFI_PASS;
const char* pcApiUrl = "http://192.168.31.31:5000/api/pc"; 
const long  gmtOffset_sec = 3 * 3600;
const int   daylightOffset_sec = 0;
const char* ntpServer = "pool.ntp.org";

const char* www_login = WWW_LOGIN;
const char* www_pass = WWW_PASS;
uint8_t targetMac[] = PC_MAC_BYTES;

unsigned long lastActivityTime = 0;
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, 22, 21);
BleKeyboard bleKeyboard("BLE Vault", "ESP32", 100);
WebServer server(80);
WiFiUDP pingUdp;
const unsigned int localUdpPort = 4210;
char packetBuffer[32];
bool networkInitialized = false;

enum AppState {
    STATE_DASHBOARD,
    STATE_PIN_ENTRY,
    STATE_ERROR,
    STATE_VAULT,
    STATE_FILE_SELECT,
    STATE_TEXT_READER,
    STATE_MEDIA_CTRL,
    STATE_BRIGHTNESS_CTRL
};
AppState currentState = STATE_DASHBOARD;

volatile int encoderPos = 0;
volatile int lastEncoded = 0;
volatile int rawTicks = 0;
int currentSelection = 0;
bool redrawRequired = true;
int brightD1 = 20;
int brightD2 = 20;
int brightSelection = 0;

const int MAX_SCREENS = 11;
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

const uint8_t MAX_RECORDS = 20;
String serviceNames[MAX_RECORDS];
String passwords[MAX_RECORDS];
uint8_t totalRecords = 0;
int pinDigits[4] = {0, 0, 0, 0};
int currentPinIndex = 0;
String errorMessage = "";

std::vector<String> textLines;
int currentTopLine = 0;

std::vector<String> fileList;
int currentFileIndex = 0;

const int GRAPH_WIDTH = 30; 
int pingHistory[GRAPH_WIDTH];
int currentPing = 0;

File uploadFile;

void handleRoot() {
    if (!server.authenticate(www_login, www_pass)) return server.requestAuthentication();
    
    String html;
    html.reserve(2048);
    
    html += "<html><head><meta charset='utf-8'></head><body><h2>SD Manager</h2>";
    html += "<form method='POST' action='/upload' enctype='multipart/form-data'><input type='file' name='f'><input type='submit' value='Upload'></form><hr><ul>";
    
    File root = SD.open("/");
    if (!root) {
        html += "<li>Ошибка чтения SD-карты</li></ul></body></html>";
        server.sendHeader("Connection", "close");
        server.send(200, "text/html", html);
        return;
    }

    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            const char* name = file.name();
            if (name != nullptr) {
                String fileName = String(name);
                if (!fileName.startsWith(".") && !fileName.startsWith("System") && !fileName.startsWith("/System")) {
                    html += "<li>" + fileName + " - <a href='/del?f=" + fileName + "'>DEL</a></li>";
                }
            }
        }
        file.close(); 
        file = root.openNextFile();
    }
    root.close();
    
    html += "</ul></body></html>";
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", html);
}

void handleDel() {
    if (!server.authenticate(www_login, www_pass)) return server.requestAuthentication();
    if (server.hasArg("f")) {
        String filename = server.arg("f");
        if (!filename.startsWith("/")) filename = "/" + filename;
        
        server.sendHeader("Location", "/");
        server.sendHeader("Connection", "close");
        server.send(303);
        
        vTaskDelay(50 / portTICK_PERIOD_MS);
        SD.remove(filename);
    }
}

void handleUpload() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        String filename = upload.filename;
        if (!filename.startsWith("/")) filename = "/" + filename;
        SD.remove(filename);
        uploadFile = SD.open(filename, FILE_WRITE);
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (uploadFile) {
            uploadFile.write(upload.buf, upload.currentSize);
            yield();
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (uploadFile) {
            uploadFile.close();
            vTaskDelay(100 / portTICK_PERIOD_MS);
        }
    }
}

void sendWakeOnLan() {
    WiFiUDP wolUdp;
    wolUdp.beginPacket(IPAddress(192, 168, 31, 255), 9);
    for (int i = 0; i < 6; i++) wolUdp.write(0xFF);
    for (int i = 0; i < 16; i++) wolUdp.write(targetMac, 6);
    wolUdp.endPacket();
}

void IRAM_ATTR encoderISR() {
    int MSB = digitalRead(PIN_ENC_S1);
    int LSB = digitalRead(PIN_ENC_S2);
    int encoded = (MSB << 1) | LSB;
    int sum = (lastEncoded << 2) | encoded;
    
    if (sum == 0b1101 || sum == 0b0100 || sum == 0b0010 || sum == 0b1011) rawTicks++;
    if (sum == 0b1110 || sum == 0b0111 || sum == 0b0001 || sum == 0b1000) rawTicks--;

    lastEncoded = encoded;

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
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_15, 0);
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
        http.setTimeout(500);
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

void loadFileList() {
    fileList.clear();
    File root = SD.open("/");
    if (!root) return;
    
    File file = root.openNextFile();
    while (file) {
        if (!file.isDirectory()) {
            String fileName = String(file.name());
            String fileNameLower = fileName;
            fileNameLower.toLowerCase();
            if (fileNameLower.endsWith(".txt")) {
                fileList.push_back(fileName);
            }
        }
        file.close();
        file = root.openNextFile();
    }
    root.close();
}

void loadText(const char* filename) {
    textLines.clear();
    File f = SD.open(filename);
    if (!f) {
        textLines.push_back("Ошибка файла");
        return;
    }

    String currentLine = "";
    String currentWord = "";
    int lineLen = 0;
    int wordLen = 0;

    while (f.available()) {
        char c = f.read();
        if (c == '\r') continue;
        bool isUtfStartOrAscii = ((c & 0xC0) != 0x80);

        if (c == ' ' || c == '\n') {
            if (lineLen + wordLen > 21) {
                textLines.push_back(currentLine);
                currentLine = currentWord;
                lineLen = wordLen;
            } else {
                currentLine += currentWord;
                lineLen += wordLen;
            }
            currentWord = "";
            wordLen = 0;
            
            if (c == '\n') {
                textLines.push_back(currentLine);
                currentLine = "";
                lineLen = 0;
            } else {
                currentLine += " ";
                lineLen++;
                if (lineLen >= 21) {
                    textLines.push_back(currentLine);
                    currentLine = "";
                    lineLen = 0;
                }
            }
        } else {
            currentWord += c;
            if (isUtfStartOrAscii) wordLen++;
            
            if (wordLen > 21) {
                textLines.push_back(currentWord);
                currentWord = "";
                wordLen = 0;
            }
        }
    }
  
    if (currentWord.length() > 0) {
        if (lineLen + wordLen > 21) {
            textLines.push_back(currentLine);
            currentLine = currentWord;
        } else {
            currentLine += currentWord;
        }
    }
    if (currentLine.length() > 0) textLines.push_back(currentLine);
    f.close();
}
void sendBrightness() {
    if (WiFi.status() == WL_CONNECTED) {
        WiFiClient client;
        HTTPClient http;
        http.begin(client, "http://192.168.31.31:5000/api/brightness");
        http.addHeader("Content-Type", "application/json");
        char json[64];
        snprintf(json, sizeof(json), "{\"d1\":%d,\"d2\":%d}", brightD1, brightD2);
        
        Serial.print("Отправка яркости: ");
        Serial.println(json);
        
        int httpCode = http.POST(json);
        
        Serial.print("Код ответа сервера: ");
        Serial.println(httpCode);
        
        if (httpCode > 0) {
            Serial.println(http.getString());
        }
        
        http.end();
    } else {
        Serial.println("Ошибка: WiFi не подключен при отправке яркости");
    }
}

bool decryptVault() {
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
    plainText.reserve(validDataSize + 1);
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
    Serial.begin(115200);
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) delay(500);

    pinMode(PIN_ENC_S1, INPUT_PULLUP);
    pinMode(PIN_ENC_S2, INPUT_PULLUP);
    pinMode(PIN_ENC_BTN, INPUT_PULLUP);

    attachInterrupt(digitalPinToInterrupt(PIN_ENC_S1), encoderISR, CHANGE);
    attachInterrupt(digitalPinToInterrupt(PIN_ENC_S2), encoderISR, CHANGE);

    u8g2.begin();
    u8g2.enableUTF8Print();
    u8g2.setPowerSave(0);

    for (int i = 0; i < GRAPH_WIDTH; i++) pingHistory[i] = -1;

    if (!SD.begin(PIN_SD_CS)) {
        u8g2.clearBuffer();
        u8g2.setFont(u8g2_font_6x13_t_cyrillic);
        u8g2.drawUTF8(10, 30, "Ошибка SD карты!");
        u8g2.sendBuffer();
        delay(2000);
    }

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x13_t_cyrillic);
    u8g2.drawUTF8(10, 30, "Поиск Wi-Fi...");
    u8g2.sendBuffer();
    
    WiFi.begin(ssid, password);
    int retries = 0;
    while (WiFi.status() != WL_CONNECTED && retries < 10) { 
        delay(500);
        retries++;
    }

    lastActivityTime = millis();
}

void drawUI() {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x13_t_cyrillic);

    if (currentState == STATE_DASHBOARD) {
        for (int i = 0; i < MAX_SCREENS; i++) {
            int x_pos = 16 + i * 10; 
            if (i == currentSelection) u8g2.drawDisc(x_pos, 8, 3);
            else u8g2.drawCircle(x_pos, 8, 3);
        }

        switch (currentSelection) {
            case 0:
                u8g2.setCursor(0, 30); u8g2.print("WiFi: "); 
                if (WiFi.status() == WL_CONNECTED) {
                    u8g2.print(WiFi.SSID());
                    u8g2.setCursor(0, 45); u8g2.print("IP: "); u8g2.print(WiFi.localIP());
                } else u8g2.print("ОФЛАЙН");
                break;
            case 1:
                u8g2.setFont(u8g2_font_logisoso16_tf);
                u8g2.setCursor(25, 45); u8g2.print(timeData);
                break;
            case 2:
                u8g2.setCursor(0, 25); u8g2.print("CPU: "); u8g2.print(pcCpu); u8g2.print(" %");
                u8g2.setCursor(0, 40); u8g2.print("GPU: "); u8g2.print(pcGpu); u8g2.print(" %");
                u8g2.setCursor(0, 55); u8g2.print("RAM: "); u8g2.print(pcRam);
                break;
            case 3:
                u8g2.setFont(u8g2_font_logisoso16_tf);
                u8g2.setCursor(0, 35); u8g2.print(pcWeatherTemp);
                u8g2.setFont(u8g2_font_6x13_t_cyrillic);
                u8g2.setCursor(0, 55); u8g2.print(pcWeatherDesc);
                break;
            case 4:
                u8g2.setCursor(0, 25); u8g2.print("Свободно: "); u8g2.print(pcDisk); u8g2.print(" Gb");
                u8g2.setCursor(0, 40); u8g2.print("Трек:");
                u8g2.setCursor(0, 55); u8g2.print(pcMedia);
                break;
            case 5:
                u8g2.drawBox(5, 30, 118, 16);
                u8g2.setDrawColor(0);
                u8g2.drawUTF8(15, 42, "Менеджер паролей");
                u8g2.setDrawColor(1);
                break;
            case 6:
                u8g2.drawBox(5, 30, 118, 16);
                u8g2.setDrawColor(0);
                u8g2.drawUTF8(15, 42, "Читалка txt");
                u8g2.setDrawColor(1);
                break;
            case 7: {
                u8g2.setCursor(0, 20);

                int maxP = -1, minP = 9999;
                for (int i = 0; i < GRAPH_WIDTH; i++) {
                    if (pingHistory[i] != -1 && pingHistory[i] < 999) {
                        if (pingHistory[i] > maxP) maxP = pingHistory[i];
                        if (pingHistory[i] < minP) minP = pingHistory[i];
                    }
                }
                if (maxP == -1) { maxP = 50; minP = 0; }
                int amp = maxP - minP;
                int pad = (amp < 10) ? 5 : amp / 4;
                int gMax = maxP + pad, gMin = (minP - pad < 0) ? 0 : minP - pad;
                int gBottom = 63, gTop = 16, graphXStart = 25;

                u8g2.setCursor(0, gTop + 10); u8g2.print(gMax);
                u8g2.setCursor(0, gBottom); u8g2.print(gMin);
                u8g2.drawVLine(graphXStart - 2, gTop, gBottom - gTop + 1);

                for (int i = 1; i < GRAPH_WIDTH; i++) {
                    if (pingHistory[i] == -1) continue;
                    int x0 = map(i - 1, 0, GRAPH_WIDTH - 1, graphXStart, 127);
                    int x1 = map(i, 0, GRAPH_WIDTH - 1, graphXStart, 127);
                    int y0 = map(constrain(pingHistory[i-1] == -1 ? pingHistory[i] : pingHistory[i-1], gMin, gMax), gMin, gMax, gBottom, gTop);
                    int y1 = map(constrain(pingHistory[i], gMin, gMax), gMin, gMax, gBottom, gTop);
                    u8g2.drawLine(x0, y0, x1, y1);
                }
                break;
            }
            case 8:
                u8g2.setCursor(0, 30); u8g2.print("MAC: " PC_MAC_STR);
                u8g2.drawBox(5, 40, 118, 16);
                u8g2.setDrawColor(0);
                u8g2.drawUTF8(10, 52, "Разбудить ПК");
                u8g2.setDrawColor(1);
                break;
            case 9:
                u8g2.drawBox(5, 30, 118, 16);
                u8g2.setDrawColor(0);
                u8g2.drawUTF8(10, 42, "Медиа пульт");
                u8g2.setDrawColor(1);
                break;
            case 10:
                u8g2.drawBox(5, 30, 118, 16);
                u8g2.setDrawColor(0);
                u8g2.drawUTF8(10, 42, "Яркость экранов");
                u8g2.setDrawColor(1);
                break;
        }
    } else if (currentState == STATE_FILE_SELECT) {
        u8g2.drawUTF8(0, 10, "Выбор файла");
        u8g2.drawLine(0, 13, 128, 13);
        
        if (fileList.empty()) {
            u8g2.drawUTF8(5, 35, "Файлы не найдены");
        } else {
            int startIdx = currentFileIndex > 2 ? currentFileIndex - 2 : 0;
            for (int i = 0; i < 3; i++) {
                int itemIdx = startIdx + i;
                if (itemIdx < fileList.size()) {
                    int yPos = 28 + (i * 15);
                    if (itemIdx == currentFileIndex) {
                        u8g2.drawBox(0, yPos - 11, 128, 14);
                        u8g2.setDrawColor(0);
                    } else u8g2.setDrawColor(1);
                    
                    u8g2.setCursor(5, yPos);
                    String dispName = fileList[itemIdx];
                    if (dispName.startsWith("/")) dispName = dispName.substring(1);
                    u8g2.print(dispName);
                    u8g2.setDrawColor(1);
                }
            }
        }
    } else if (currentState == STATE_TEXT_READER) {
        u8g2.setCursor(0, 10);
        String fName = fileList[currentFileIndex];
        if (fName.startsWith("/")) fName = fName.substring(1);
        u8g2.print(fName);
        
        for (int i = 0; i < 5; i++) {
            int lineIdx = currentTopLine + i;
            if (lineIdx < textLines.size()) {
                u8g2.setCursor(0, 26 + (i * 10));
                u8g2.print(textLines[lineIdx]);
            }
        }
    } else if (currentState == STATE_PIN_ENTRY) {
        u8g2.drawUTF8(20, 15, "Введите ПИН-код");
        for (int i = 0; i < 4; i++) {
            int x = 30 + (i * 18);
            if (i == currentPinIndex) {
                u8g2.drawBox(x - 2, 25, 12, 16);
                u8g2.setDrawColor(0);
            } else u8g2.setDrawColor(1);
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
            if (itemIdx <= totalRecords) { 
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
    } else if (currentState == STATE_MEDIA_CTRL) {
        if (!bleKeyboard.isConnected()) u8g2.drawUTF8(0, 10, "Ожидание BLE...");
        else u8g2.drawUTF8(0, 10, "BLE Подключен");
        u8g2.setFont(u8g2_font_logisoso16_tf);
        u8g2.setCursor(20, 35); u8g2.print("VOLUME");
        u8g2.setFont(u8g2_font_6x13_t_cyrillic);
        u8g2.drawUTF8(10, 60, "Удерж. -> Выход");
    } else if (currentState == STATE_BRIGHTNESS_CTRL) {
        u8g2.drawUTF8(0, 10, "Яркость");
        u8g2.drawLine(0, 13, 128, 13);
        
        u8g2.setCursor(10, 30);
        if (brightSelection == 0) u8g2.print("> ");
        u8g2.print("Дисплей 1: "); u8g2.print(brightD1);

        u8g2.setCursor(10, 45);
        if (brightSelection == 1) u8g2.print("> ");
        u8g2.print("Дисплей 2: "); u8g2.print(brightD2);
        
        u8g2.drawUTF8(0, 60, "Клик: след / Удерж: ОК");
    }
    u8g2.sendBuffer();
}

void loop() {
    if (millis() - lastActivityTime > SLEEP_TIMEOUT_MS) goToDeepSleep();

    // Сетевой стек работает ТОЛЬКО если Bluetooth выключен
    if (WiFi.status() == WL_CONNECTED && currentState != STATE_MEDIA_CTRL && currentState != STATE_VAULT) {
        if (!networkInitialized) {
            pingUdp.begin(localUdpPort);
            server.on("/", HTTP_GET, handleRoot);
            server.on("/upload", HTTP_POST, [](){
                server.sendHeader("Connection", "close");
                server.sendHeader("Location", "/");
                server.send(303, "text/plain", "");
            }, handleUpload);
            server.on("/del", HTTP_GET, handleDel);
            server.begin();
            server.client().setNoDelay(true);
            configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
            networkInitialized = true;
        }
        
        server.handleClient();

        int packetSize = pingUdp.parsePacket();
        if (packetSize) {
            int len = pingUdp.read(packetBuffer, 31);
            if (len > 0) packetBuffer[len] = 0;
            currentPing = String(packetBuffer).toInt();
            for (int i = 0; i < GRAPH_WIDTH - 1; i++) pingHistory[i] = pingHistory[i + 1];
            pingHistory[GRAPH_WIDTH - 1] = currentPing;
            if (currentState == STATE_DASHBOARD && currentSelection == 7) redrawRequired = true;
        }
    } else if (WiFi.status() != WL_CONNECTED) {
        networkInitialized = false; 
    }

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

    int newPos = encoderPos;
    static bool btnState = HIGH;
    static unsigned long btnPressTime = 0;
    bool btnCurrent = digitalRead(PIN_ENC_BTN);
    bool shortPress = false;
    bool longPress = false;

    if (btnCurrent == LOW && btnState == HIGH) {
        btnPressTime = millis();
        btnState = LOW;
    } else if (btnCurrent == HIGH && btnState == LOW) {
        unsigned long duration = millis() - btnPressTime;
        btnState = HIGH;
        if (duration > 1000) longPress = true;
        else if (duration > 50) shortPress = true;
    }

    if (shortPress || longPress) lastActivityTime = millis();

    if (currentState == STATE_DASHBOARD) {
        if (newPos != currentSelection) {
            lastActivityTime = millis();
            if (newPos >= MAX_SCREENS) { newPos = 0; encoderPos = 0; } 
            else if (newPos < 0) { newPos = MAX_SCREENS - 1; encoderPos = MAX_SCREENS - 1; }
            currentSelection = newPos;
            redrawRequired = true;
        }

        if (shortPress) {
            if (currentSelection == 5) {
                currentState = STATE_PIN_ENTRY;
                currentPinIndex = 0;
                encoderPos = 0;
                for (int i = 0; i < 4; i++) pinDigits[i] = 0;
            } else if (currentSelection == 6) {
                currentState = STATE_FILE_SELECT;
                loadFileList();
                currentFileIndex = 0;
                encoderPos = 0;
            } else if (currentSelection == 8) {
                sendWakeOnLan();
                u8g2.clearBuffer();
                u8g2.drawUTF8(10, 35, "Пакет отправлен!");
                u8g2.sendBuffer();
                delay(1000);
            } else if (currentSelection == 9) {
                // Аппаратное отключение Wi-Fi и запуск BLE
                u8g2.clearBuffer();
                u8g2.drawUTF8(10, 35, "Запуск BLE...");
                u8g2.sendBuffer();
                
                WiFi.disconnect(true);
                WiFi.mode(WIFI_OFF);
                delay(100);
                bleKeyboard.begin();
                
                currentState = STATE_MEDIA_CTRL;
                encoderPos = 0;
            } else if (currentSelection == 10) {
                currentState = STATE_BRIGHTNESS_CTRL;
                encoderPos = brightSelection == 0 ? brightD1 : brightD2; 
            redrawRequired = true;
            }
        }
    } else if (currentState == STATE_FILE_SELECT) {
        if (newPos != currentFileIndex) {
            lastActivityTime = millis();
            if (!fileList.empty()) {
                if (newPos >= (int)fileList.size()) { newPos = 0; encoderPos = 0; }
                else if (newPos < 0) { newPos = fileList.size() - 1; encoderPos = fileList.size() - 1; }
                currentFileIndex = newPos;
                redrawRequired = true;
            } else encoderPos = 0;
        }

        if (shortPress && !fileList.empty()) {
            currentState = STATE_TEXT_READER;
            String path = fileList[currentFileIndex];
            if (!path.startsWith("/")) path = "/" + path;
            loadText(path.c_str());
            encoderPos = 0;
            currentTopLine = 0;
            redrawRequired = true;
        }

        if (longPress) { 
            currentState = STATE_DASHBOARD;
            currentSelection = 6;
            encoderPos = 6;
            fileList.clear(); 
            redrawRequired = true;
        }
    } else if (currentState == STATE_TEXT_READER) {
        if (newPos != currentTopLine) {
            lastActivityTime = millis();
            int maxScroll = 0;
            if (textLines.size() > 5) maxScroll = textLines.size() - 5;
            if (newPos > maxScroll) newPos = maxScroll;
            if (newPos < 0) newPos = 0;
            encoderPos = newPos;
            currentTopLine = newPos;
            redrawRequired = true;
        }

        if (longPress) { 
            currentState = STATE_FILE_SELECT;
            textLines.clear(); 
            encoderPos = currentFileIndex;
            redrawRequired = true;
        }
    } else if (currentState == STATE_MEDIA_CTRL) {
        if (newPos != 0) {
            lastActivityTime = millis();
            if (bleKeyboard.isConnected()) {
                if (newPos > 0) bleKeyboard.write(KEY_MEDIA_VOLUME_UP);
                else bleKeyboard.write(KEY_MEDIA_VOLUME_DOWN);
            }
            encoderPos = 0;
        }
        if (shortPress && bleKeyboard.isConnected()) {
            bleKeyboard.write(KEY_MEDIA_PLAY_PAUSE);
        }
        if (longPress) {
            // Аппаратный сброс для гарантированной очистки стека BLE
            u8g2.clearBuffer();
            u8g2.drawUTF8(10, 35, "Возврат...");
            u8g2.sendBuffer();
            delay(300);
            esp_restart(); 
        }
    } else if (currentState == STATE_PIN_ENTRY) {
        if (newPos != pinDigits[currentPinIndex]) {
            lastActivityTime = millis();
            if (newPos > 9) { newPos = 0; encoderPos = 0; } 
            else if (newPos < 0) { newPos = 9; encoderPos = 9; }
            pinDigits[currentPinIndex] = newPos;
            redrawRequired = true;
        }

        if (shortPress) {
            currentPinIndex++;
            if (currentPinIndex > 3) {
                u8g2.clearBuffer();
                u8g2.drawUTF8(20, 35, "Расшифровка...");
                u8g2.sendBuffer();
                
                if (decryptVault()) {
                    // Аппаратное отключение Wi-Fi и запуск BLE
                    u8g2.clearBuffer();
                    u8g2.drawUTF8(10, 35, "Запуск BLE...");
                    u8g2.sendBuffer();
                    
                    WiFi.disconnect(true);
                    WiFi.mode(WIFI_OFF);
                    delay(100);
                    bleKeyboard.begin();
                    
                    currentState = STATE_VAULT;
                    encoderPos = 0;
                    currentSelection = 0;
                } else currentState = STATE_ERROR;
            } else encoderPos = pinDigits[currentPinIndex];
            redrawRequired = true;
        }
    } else if (currentState == STATE_ERROR) {
        if (shortPress) {
            currentState = STATE_DASHBOARD;
            currentSelection = 5;
            encoderPos = 5;
            redrawRequired = true;
        }
    } else if (currentState == STATE_VAULT) {
        if (newPos != currentSelection) {
            lastActivityTime = millis();
            if (newPos > totalRecords) { newPos = 0; encoderPos = 0; } 
            else if (newPos < 0) { newPos = totalRecords; encoderPos = totalRecords; }
            currentSelection = newPos;
            redrawRequired = true;
        }

        if (shortPress) {
            if (currentSelection == totalRecords) {
                // Аппаратный сброс для гарантированной очистки стека BLE
                u8g2.clearBuffer();
                u8g2.drawUTF8(10, 35, "Возврат...");
                u8g2.sendBuffer();
                delay(300);
                esp_restart();
            } else if (bleKeyboard.isConnected() && passwords[currentSelection].length() > 0) {
                bleKeyboard.print(passwords[currentSelection]);
                u8g2.clearBuffer();
                u8g2.drawUTF8(10, 35, "Пароль отправлен!");
                u8g2.sendBuffer();
                delay(1000);
            }
            redrawRequired = true;
        }
    } else if (currentState == STATE_BRIGHTNESS_CTRL) {
        int* currentVal = brightSelection == 0 ? &brightD1 : &brightD2;
        if (newPos != *currentVal) {
            lastActivityTime = millis();
            if (newPos > 100) newPos = 100;
            if (newPos < 0) newPos = 0;
            *currentVal = newPos;
            encoderPos = newPos;
            redrawRequired = true;
        }

        if (shortPress) {
            brightSelection = !brightSelection;
            encoderPos = brightSelection == 0 ? brightD1 : brightD2;
            redrawRequired = true;
        }

        if (longPress) {
            currentState = STATE_DASHBOARD;
            currentSelection = 10;
            encoderPos = 10;
            sendBrightness();
            redrawRequired = true;
        }
    }

    if (redrawRequired) {
        drawUI();
        redrawRequired = false;
    }
    delay(10);
}
