#include <driver/i2s.h>

// Переназначенные нейтральные пины
#define I2S_WS 25
#define I2S_SCK 26
#define I2S_SD 33
#define I2S_PORT I2S_NUM_0
#define BUFFER_SIZE 256

int32_t sBuffer[BUFFER_SIZE];

void setup() {
  Serial.begin(115200);
  Serial.println("Starting I2S Test on new pins...");

  const i2s_config_t i2s_config = {
    .mode = i2s_mode_t(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = 16000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = i2s_comm_format_t(I2S_COMM_FORMAT_STAND_I2S),
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = 64,
    .use_apll = false,
    .tx_desc_auto_clear = false,
    .fixed_mclk = 0
  };

  const i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = I2S_PIN_NO_CHANGE,
    .data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pin_config);
  Serial.println("I2S Init Done.");
}

void loop() {
  size_t bytes_read;
  i2s_read(I2S_PORT, &sBuffer, sizeof(sBuffer), &bytes_read, portMAX_DELAY);
  
  int samples_read = bytes_read / sizeof(int32_t);
  
  if (samples_read > 0) {
    uint64_t sum = 0;
    for (int i = 0; i < samples_read; i++) {
      int32_t sample = sBuffer[i] >> 14; 
      sum += (int64_t)sample * sample;
    }
    float rms = sqrt((float)sum / samples_read);
    
    Serial.print("Raw:");
    Serial.print(sBuffer[0]);
    Serial.print("\tRMS:");
    Serial.println(rms);
  }
}
