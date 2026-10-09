#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <SPI.h>
#include <SD.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define SD_CS 5

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
File vFile;

void setup() {
  Wire.begin();
  Wire.setClock(800000); // Разгон шины I2C
  
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) for(;;);
  
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  
  if(!SD.begin(SD_CS)) {
    display.print("SD Err");
    display.display();
    for(;;);
  }
  
  vFile = SD.open("/video.bin");
  if(!vFile) {
    display.print("No video.bin");
    display.display();
    for(;;);
  }
}

void loop() {
  if (vFile.available() >= 1024) {
    vFile.read(display.getBuffer(), 1024);
    display.display();
  } else {
    vFile.seek(0); // Зациклить видео по окончании
  }
}
