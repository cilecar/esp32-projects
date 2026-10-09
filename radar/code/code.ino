#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3C

#define PIN_PIR 14
#define PIN_TRIG 13
#define PIN_ECHO 26 // Безопасный пин с делителем напряжения
#define PIN_SERVO 27

#define DISTANCE_THRESHOLD_CM 20.0 // Измененный порог срабатывания (20 см)
#define SERVO_MIN_ANGLE 0
#define SERVO_MAX_ANGLE 180
#define SERVO_IDLE_ANGLE 90

#define SCAN_TIMEOUT_MS 5000 // Время сканирования после потери движения

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Servo radarServo;

enum SystemState {
    STATE_IDLE,
    STATE_SCANNING,
    STATE_TRACKING
};

SystemState currentState = STATE_IDLE;
int currentAngle = SERVO_IDLE_ANGLE;
int scanDirection = 1;
float lastDistance = 0.0;

unsigned long lastServoTime = 0;
unsigned long lastMeasureTime = 0;
unsigned long lastDisplayTime = 0;
unsigned long lastMotionTime = 0;

void setup() {
    pinMode(PIN_PIR, INPUT);
    pinMode(PIN_TRIG, OUTPUT);
    pinMode(PIN_ECHO, INPUT);

    radarServo.setPeriodHertz(50);
    radarServo.attach(PIN_SERVO, 500, 2400);
    radarServo.write(currentAngle);

    Wire.begin(21, 22);
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        while (true);
    }
    
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    updateDisplay();
}

void loop() {
    if (digitalRead(PIN_PIR)) {
        lastMotionTime = millis();
    }
    
    bool isScanningActive = (millis() - lastMotionTime < SCAN_TIMEOUT_MS);

    measureDistance();

    switch (currentState) {
        case STATE_IDLE:
            if (isScanningActive) {
                currentState = STATE_SCANNING;
            } else if (currentAngle != SERVO_IDLE_ANGLE) {
                currentAngle = SERVO_IDLE_ANGLE;
                radarServo.write(currentAngle);
            }
            break;

        case STATE_SCANNING:
            if (!isScanningActive) {
                currentState = STATE_IDLE;
            } else if (lastDistance > 0 && lastDistance < DISTANCE_THRESHOLD_CM) {
                currentState = STATE_TRACKING;
            } else {
                updateServoScan();
            }
            break;

        case STATE_TRACKING:
            if (!isScanningActive) {
                currentState = STATE_IDLE;
            } else if (lastDistance >= DISTANCE_THRESHOLD_CM || lastDistance == 0) {
                currentState = STATE_SCANNING;
            }
            break;
    }

    if (millis() - lastDisplayTime > 100) {
        updateDisplay();
        lastDisplayTime = millis();
    }
}

void measureDistance() {
    if (millis() - lastMeasureTime < 50) return;
    
    digitalWrite(PIN_TRIG, LOW);
    delayMicroseconds(2);
    digitalWrite(PIN_TRIG, HIGH);
    delayMicroseconds(10);
    digitalWrite(PIN_TRIG, LOW);

    long duration = pulseIn(PIN_ECHO, HIGH, 30000);
    
    if (duration == 0) {
        lastDistance = 0;
    } else {
        lastDistance = (duration / 2.0) / 29.1;
    }
    
    lastMeasureTime = millis();
}

void updateServoScan() {
    if (millis() - lastServoTime < 20) return;

    currentAngle += scanDirection;
    if (currentAngle >= SERVO_MAX_ANGLE) {
        currentAngle = SERVO_MAX_ANGLE;
        scanDirection = -1;
    } else if (currentAngle <= SERVO_MIN_ANGLE) {
        currentAngle = SERVO_MIN_ANGLE;
        scanDirection = 1;
    }
    
    radarServo.write(currentAngle);
    lastServoTime = millis();
}

void updateDisplay() {
    display.clearDisplay();
    display.setCursor(0, 0);

    display.print("MODE: ");
    switch (currentState) {
        case STATE_IDLE: display.println("IDLE"); break;
        case STATE_SCANNING: display.println("SCAN"); break;
        case STATE_TRACKING: display.println("LOCK"); break;
    }

    display.print("PIR:  ");
    display.println(digitalRead(PIN_PIR) ? "MOTION" : "WAIT");

    display.print("DIST: ");
    if (lastDistance == 0) {
        display.println("---");
    } else {
        display.print(lastDistance, 1);
        display.println(" cm");
    }

    display.print("ANGL: ");
    display.print(currentAngle);
    display.println(" deg");

    if (currentState == STATE_TRACKING) {
        display.fillCircle(110, 10, 5, SSD1306_WHITE);
    } else if (currentState == STATE_SCANNING) {
        display.drawCircle(110, 10, 5, SSD1306_WHITE);
    }

    display.display();
}
