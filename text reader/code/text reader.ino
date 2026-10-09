#include <GyverOLED.h>
#include <SPI.h>
#include <SD.h>
#include <vector>

#define SD_CS 5
const int PIN_CLK = 25;
const int PIN_DT = 26;

GyverOLED<SSD1306_128x64, OLED_BUFFER> oled;

std::vector<String> textLines;
int currentTopLine = 0;

volatile int encoderRaw = 0;
volatile uint8_t old_AB = 0;
const int8_t enc_states[] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

void IRAM_ATTR readEncoder() {
  old_AB <<= 2;
  old_AB |= ((digitalRead(PIN_DT) << 1) | digitalRead(PIN_CLK));
  encoderRaw += enc_states[(old_AB & 0x0F)];
}

void loadText(const char* filename) {
  File f = SD.open(filename);
  if (!f) return;

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

void drawPage() {
  oled.clear();
  
  oled.setCursor(0, 0);
  oled.print(F(" ФАЙЛ: text.txt"));
  
  for (int i = 0; i < 6; i++) {
    int lineIdx = currentTopLine + i;
    if (lineIdx < textLines.size()) {
      oled.setCursor(0, i + 2);
      oled.print(textLines[lineIdx]);
    }
  }
  
  oled.update();
}

void setup() {
  pinMode(PIN_CLK, INPUT_PULLUP);
  pinMode(PIN_DT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(PIN_CLK), readEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_DT), readEncoder, CHANGE);

  oled.init();
  oled.clear();
  oled.update();
  
  if (!SD.begin(SD_CS)) {
    oled.setCursor(0, 0);
    oled.print(F("SD Error"));
    oled.update();
    for(;;);
  }
  
  loadText("/text.txt");
  drawPage();
}

void loop() {
  static int lastPos = 0;
  int currentPos = encoderRaw / 4;
  
  if (currentPos != lastPos) {
    currentTopLine -= (currentPos - lastPos);
    
    int maxScroll = 0;
    if (textLines.size() > 6) {
      maxScroll = textLines.size() - 6;
    }
    
    currentTopLine = constrain(currentTopLine, 0, maxScroll);
    
    lastPos = currentPos;
    encoderRaw = lastPos * 4;
    
    drawPage();
  }
}
