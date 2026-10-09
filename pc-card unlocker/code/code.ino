#include "secrets.h"
#include <SPI.h>
#include <MFRC522.h>
#include <BleKeyboard.h>

#define RST_PIN 4
#define SS_PIN  5

MFRC522 mfrc522(SS_PIN, RST_PIN);
BleKeyboard bleKeyboard("ESP32 Unlocker", "DIY", 100);

const String MY_KEY_UID = CARD_UID;
const char* MY_PASSWORD = PC_PASSWORD;

void setup() {
  Serial.begin(115200);
  
  SPI.begin();
  mfrc522.PCD_Init();
  
  Serial.println("Запуск Bluetooth...");
  bleKeyboard.begin();
}

void loop() {
  // Ждем, пока плата не подключится к компу по Bluetooth
  if (!bleKeyboard.isConnected()) {
    delay(500);
    return;
  }

  if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
    return;
  }

  String content = "";
  for (byte i = 0; i < mfrc522.uid.size; i++) {
     content += String(mfrc522.uid.uidByte[i] < 0x10 ? "0" : "");
     content += String(mfrc522.uid.uidByte[i], HEX);
  }
  content.toUpperCase();
  
  Serial.println("Считан UID: " + content);

  if (content == MY_KEY_UID) {
    Serial.println("Разблокировка...");
    
    bleKeyboard.print(MY_PASSWORD);
    bleKeyboard.write(KEY_RETURN);
    
    delay(3000); 
  } else {
    Serial.println("Чужая метка!");
    delay(1000);
  }

  mfrc522.PICC_HaltA();
}
