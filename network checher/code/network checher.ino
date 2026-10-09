#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define GRAPH_X_START 25 

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

const int GRAPH_WIDTH = 30; 
int pingHistory[GRAPH_WIDTH];
int currentPing = 0;

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(50); 

  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    for(;;); 
  }

  for (int i = 0; i < GRAPH_WIDTH; i++) {
    pingHistory[i] = -1;
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.display();
}

void loop() {
  if (Serial.available() > 0) {
    String incoming = Serial.readStringUntil('\n');
    currentPing = incoming.toInt();

    for (int i = 0; i < GRAPH_WIDTH - 1; i++) {
      pingHistory[i] = pingHistory[i + 1];
    }
    pingHistory[GRAPH_WIDTH - 1] = currentPing;

    drawDashboard();
  }
}

void drawDashboard() {
  display.clearDisplay();

  // Желтая зона (0-15)
  display.setCursor(0, 0);
  display.print("VPN:");
  display.setCursor(30, 0);
  if (currentPing >= 999) {
    display.print("TIMEOUT");
  } else {
    display.print(currentPing);
    display.print("ms");
  }

  int maxP = -1;
  int minP = 9999;

  for (int i = 0; i < GRAPH_WIDTH; i++) {
    if (pingHistory[i] != -1 && pingHistory[i] < 999) {
      if (pingHistory[i] > maxP) maxP = pingHistory[i];
      if (pingHistory[i] < minP) minP = pingHistory[i];
    }
  }

  if (maxP == -1) { maxP = 50; minP = 0; }

  int amp = maxP - minP;
  int pad = (amp < 10) ? 5 : amp / 4;

  int gMax = maxP + pad;
  int gMin = (minP - pad < 0) ? 0 : minP - pad;

  int gBottom = SCREEN_HEIGHT - 1;
  int gTop = 16; // ИСПРАВЛЕНО: смещено в синюю зону

  // Синяя зона (16-63)
  display.setTextSize(1);
  display.setCursor(0, gTop + 2); // ИСПРАВЛЕНО: отступ от линии графика
  display.print(gMax);
  display.setCursor(0, gBottom - 7);
  display.print(gMin);

  display.drawFastVLine(GRAPH_X_START - 2, gTop, gBottom - gTop + 1, SSD1306_WHITE);

  for (int i = 1; i < GRAPH_WIDTH; i++) {
    if (pingHistory[i] == -1) continue; 

    int x0 = map(i - 1, 0, GRAPH_WIDTH - 1, GRAPH_X_START, SCREEN_WIDTH - 1);
    int x1 = map(i, 0, GRAPH_WIDTH - 1, GRAPH_X_START, SCREEN_WIDTH - 1);

    int y0 = map(constrain(pingHistory[i-1] == -1 ? pingHistory[i] : pingHistory[i-1], gMin, gMax), gMin, gMax, gBottom, gTop);
    int y1 = map(constrain(pingHistory[i], gMin, gMax), gMin, gMax, gBottom, gTop);

    display.drawLine(x0, y0, x1, y1, SSD1306_WHITE);
  }

  display.display();
}
