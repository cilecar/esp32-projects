#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Servo myservo;

const int PIN_CLK = 18;
const int PIN_DT = 19;
const int PIN_SW = 5;
const int PIN_SERVO = 13;
const int PIN_LED_G = 14;
const int PIN_LED_R = 27;

const int SECRET_CODE[] = {1, 1, 1};
int inputCode[3] = {0, 0, 0};
int currentStep = 0;
int currentValue = 0;

volatile int encoderRaw = 0;
volatile uint8_t old_AB = 0;
const int8_t enc_states[] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

void IRAM_ATTR readEncoder() {
  old_AB <<= 2;
  old_AB |= ((digitalRead(PIN_DT) << 1) | digitalRead(PIN_CLK));
  encoderRaw += enc_states[(old_AB & 0x0F)];
}

void setup() {
  pinMode(PIN_CLK, INPUT_PULLUP);
  pinMode(PIN_DT, INPUT_PULLUP);
  pinMode(PIN_SW, INPUT_PULLUP);
  pinMode(PIN_LED_G, OUTPUT);
  pinMode(PIN_LED_R, OUTPUT);
  
  attachInterrupt(digitalPinToInterrupt(PIN_CLK), readEncoder, CHANGE);
  attachInterrupt(digitalPinToInterrupt(PIN_DT), readEncoder, CHANGE);
  
  myservo.attach(PIN_SERVO);
  myservo.write(0);
  
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) for(;;);
  display.clearDisplay();
  display.display();
  updateDisplay();
}

void loop() {
  static int lastPos = 0;
  int currentPos = encoderRaw / 4;
  
  if (currentPos != lastPos) {
    currentValue = constrain(currentPos, 0, 99);
    if (currentValue != currentPos) {
      encoderRaw = currentValue * 4;
    }
    lastPos = currentValue;
    updateDisplay();
  }

  if (digitalRead(PIN_SW) == LOW) {
    delay(200);
    inputCode[currentStep] = currentValue;
    currentStep++;
    encoderRaw = 0;
    currentValue = 0;
    lastPos = 0;
    
    if (currentStep > 2) {
      checkCode();
    } else {
      updateDisplay();
    }
    while(digitalRead(PIN_SW) == LOW);
  }
}

void updateDisplay() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0,0);
  display.println("ENTER CODE:");
  
  display.setCursor(0, 20);
  for(int i = 0; i < 3; i++) {
    if (i < currentStep) display.print("* ");
    else if (i == currentStep) {
      display.setTextSize(2);
      display.print(currentValue);
      display.setTextSize(1);
      display.print(" ");
    }
    else display.print("_ ");
  }
  display.display();
}

void checkCode() {
  bool match = true;
  for(int i = 0; i < 3; i++) {
    if(inputCode[i] != SECRET_CODE[i]) match = false;
  }

  display.clearDisplay();
  display.setCursor(0, 20);
  display.setTextSize(2);

  if(match) {
    display.println("ACCESS OK");
    display.display();
    digitalWrite(PIN_LED_G, HIGH);
    myservo.write(90);
    delay(5000);
    myservo.write(0);
    digitalWrite(PIN_LED_G, LOW);
  } else {
    display.println("WRONG!");
    display.display();
    for(int i = 0; i < 5; i++) {
      digitalWrite(PIN_LED_R, HIGH);
      delay(100);
      digitalWrite(PIN_LED_R, LOW);
      delay(100);
    }
  }
  
  currentStep = 0;
  encoderRaw = 0;
  currentValue = 0;
  updateDisplay();
}
