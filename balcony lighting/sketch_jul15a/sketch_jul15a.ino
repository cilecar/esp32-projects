#include "esp_task_wdt.h"

const int RELAY=5, PWM=6, BTN=7;
const uint32_t WDT_TIMEOUT_S = 8;         // авто-перезагрузка, если loop() зависнет
const int RELAY_ON=HIGH, RELAY_OFF=LOW;   // инвертируй если наоборот
const int FREQ=20000, RES=8;
const int FADE_MS=800;                    // время плавного перехода

const uint32_t DEBOUNCE_MS = 30;          // сколько уровень должен быть стабилен
const uint32_t STUCK_MS    = 5000;        // сколько LOW считать "залипанием"

bool state=false;

// --- дебаунс по устоявшемуся уровню ---
int rawLast=HIGH, stable=HIGH;
uint32_t rawChangedAt=0;

int cur=0, target=0;                      // текущая и целевая яркость 0..255
uint32_t lastStep=0, lastRefresh=0;
bool stuckReported=false;

uint8_t gammaTab[256];

void applyState(){
  digitalWrite(RELAY, state?RELAY_ON:RELAY_OFF);  // лампы сразу
  target = state?255:0;                           // лента плавно
}

void setup(){
  Serial.begin(115200);

  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = WDT_TIMEOUT_S*1000,
    .idle_core_mask = 0,
    .trigger_panic = true               // зависание -> перезагрузка чипа
  };
  esp_task_wdt_reconfigure(&wdtConfig);
  esp_task_wdt_add(NULL);               // подписываем текущий (loop) таск

  for(int i=0;i<256;i++) gammaTab[i]=(uint8_t)(pow(i/255.0,2.2)*255+0.5);

  pinMode(RELAY,OUTPUT);
  digitalWrite(RELAY,RELAY_OFF);
  pinMode(BTN,INPUT_PULLUP);
  ledcAttach(PWM,FREQ,RES);
  ledcWrite(PWM,0);

  stable = rawLast = digitalRead(BTN);
  Serial.printf("boot, BTN=%d\n", stable);
}

void loop(){
  esp_task_wdt_reset();                 // подтверждаем, что loop() жив
  uint32_t now = millis();

  // страховка: раз в секунду переустанавливаем режим пина
  if(now-lastRefresh >= 1000){
    lastRefresh = now;
    pinMode(BTN,INPUT_PULLUP);
  }

  int raw = digitalRead(BTN);
  if(raw != rawLast){ rawLast = raw; rawChangedAt = now; }

  // уровень изменился и продержался DEBOUNCE_MS -> принимаем
  if(raw != stable && now-rawChangedAt >= DEBOUNCE_MS){
    stable = raw;
    stuckReported = false;
    if(stable == LOW){                    // подтверждённое нажатие
      state = !state;
      applyState();
      Serial.printf("[%lu] press -> %s\n", now, state?"ON":"OFF");
    }
  }

  // диагностика: пин висит в LOW дольше, чем возможно при живой кнопке
  if(stable==LOW && !stuckReported && now-rawChangedAt >= STUCK_MS){
    stuckReported = true;
    Serial.printf("[%lu] BTN stuck LOW %lu ms\n", now, now-rawChangedAt);
  }

  // шаг плавности: проходим весь диапазон за FADE_MS
  uint32_t stepInt = FADE_MS/255;
  if(stepInt == 0) stepInt = 1;
  if(cur!=target && now-lastStep >= stepInt){
    cur += (target>cur)?1:-1;
    ledcWrite(PWM,gammaTab[cur]);
    lastStep = now;
  }

  delay(1);
}
