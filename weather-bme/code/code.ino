#include <Wire.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BMP280.h>

#define I2C_SDA 21
#define I2C_SCL 22
#define BMP280_ADDRESS 0x76 

Adafruit_BMP280 bmp;

void setup() {
  Serial.begin(115200);
  Wire.begin(I2C_SDA, I2C_SCL);

  if (!bmp.begin(BMP280_ADDRESS)) {
    Serial.println("ОШИБКА: BMP280 не найден на шине I2C!");
    while (1) delay(10);
  }
  
  Serial.println("BMP280 готов к работе!\n");
  
  // Настройка параметров выборки (oversampling) и фильтрации для повышения точности
  bmp.setSampling(Adafruit_BMP280::MODE_NORMAL,     // Режим постоянного опроса
                  Adafruit_BMP280::SAMPLING_X2,     // Oversampling для температуры
                  Adafruit_BMP280::SAMPLING_X16,    // Oversampling для давления
                  Adafruit_BMP280::FILTER_X16,      // IIR фильтр для подавления скачков от ветра/дверей
                  Adafruit_BMP280::STANDBY_MS_500); // Период обновления 500мс
}

void loop() {
  Serial.print("Температура: ");
  Serial.print(bmp.readTemperature());
  Serial.println(" *C");

  Serial.print("Давление:    ");
  Serial.print(bmp.readPressure() / 100.0F);
  Serial.println(" hPa");

  Serial.print("Высота:      ");
  Serial.print(bmp.readAltitude(1013.25)); 
  Serial.println(" m");

  Serial.println("-------------------------");
  delay(2000);
}
