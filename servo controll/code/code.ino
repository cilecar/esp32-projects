#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>

#define PIN_SERVO      18
#define PIN_LDR        33
#define PIN_BTN_OPEN   25
#define PIN_BTN_CLOSE  26
#define PIN_ENC_CLK    19
#define PIN_ENC_DT     23
#define PIN_ENC_SW     5

#define LDR_MIN 700
#define LDR_MAX 4000

Adafruit_SSD1306 display(128, 64, &Wire, -1);
Servo blindServo;

bool isAutoMode = true;
int targetAngle = 0;
float currentAngle = 0.0;
float smoothedLight = 0.0;

unsigned long lastOledUpdate = 0;
unsigned long lastServoUpdate = 0;
uint8_t lastClk = HIGH;

class Button {
  private:
    uint8_t pin;
    bool state = HIGH;
    bool lastReading = HIGH;
    unsigned long lastDebounceTime = 0;
  public:
    Button(uint8_t p) : pin(p) {}
    
    void begin() { 
      pinMode(pin, INPUT_PULLUP); 
    }
    
    bool isPressed() {
      bool reading = digitalRead(pin);
      bool pressed = false;
      
      if (reading != lastReading) {
        lastDebounceTime = millis();
      }
      
      if ((millis() - lastDebounceTime) > 50) {
        if (reading != state) {
          state = reading;
          if (state == LOW) {
            pressed = true;
          }
        }
      }
      lastReading = reading;
      return pressed;
    }
};

Button btnOpen(PIN_BTN_OPEN);
Button btnClose(PIN_BTN_CLOSE);
Button btnEncSw(PIN_ENC_SW);

void setup() {
  Serial.begin(115200);
  
  btnOpen.begin();
  btnClose.begin();
  btnEncSw.begin();
  
  pinMode(PIN_ENC_CLK, INPUT_PULLUP);
  pinMode(PIN_ENC_DT, INPUT_PULLUP);
  lastClk = digitalRead(PIN_ENC_CLK);

  ESP32PWM::allocateTimer(0);
  blindServo.setPeriodHertz(50);
  blindServo.attach(PIN_SERVO, 500, 2400);

  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    for(;;);
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
}

void loop() {
  uint8_t currentClk = digitalRead(PIN_ENC_CLK);
  if (currentClk != lastClk && currentClk == LOW) {
    isAutoMode = false;
    if (digitalRead(PIN_ENC_DT) != currentClk) {
      targetAngle += 5;
    } else {
      targetAngle -= 5;
    }
    targetAngle = constrain(targetAngle, 0, 180);
  }
  lastClk = currentClk;

  if (btnOpen.isPressed()) {
    isAutoMode = false;
    targetAngle = 0;
  }
  
  if (btnClose.isPressed()) {
    isAutoMode = false;
    targetAngle = 180;
  }
  
  if (btnEncSw.isPressed()) {
    isAutoMode = !isAutoMode; 
  }

  int rawLight = analogRead(PIN_LDR);
  smoothedLight = (0.05 * rawLight) + (0.95 * smoothedLight);
  
  int lightPercent = map(smoothedLight, LDR_MIN, LDR_MAX, 0, 100);
  lightPercent = constrain(lightPercent, 0, 100);

  if (isAutoMode) {
    int calcAngle = map(smoothedLight, LDR_MIN, LDR_MAX, 0, 180);
    calcAngle = constrain(calcAngle, 0, 180);
    if (abs(calcAngle - targetAngle) >= 2) {
      targetAngle = calcAngle;
    }
  }

  if (millis() - lastServoUpdate > 15) {
    lastServoUpdate = millis();
    if (abs(currentAngle - targetAngle) >= 1.0) {
      if (currentAngle < targetAngle) currentAngle += 1.0;
      else currentAngle -= 1.0;
      blindServo.write((int)currentAngle);
    }
  }

  if (millis() - lastOledUpdate > 100) {
    lastOledUpdate = millis();
    display.clearDisplay();
    
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print("Mode: ");
    display.print(isAutoMode ? "AUTO" : "MANUAL");

    display.setCursor(0, 20);
    display.setTextSize(2);
    display.print("Lght:");
    display.print(lightPercent);
    display.print("%");

    display.setCursor(0, 45);
    display.print("Angl:");
    display.print(targetAngle);
    
    display.display();
  }
}
