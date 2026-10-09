#include "secrets.h"
#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiUdp.h>

#define UDP_PORT  3333
#define PC_PORT   3334
#define CHUNK_SIZE 1400
#define PC_IP "192.168.31.31"

#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM   15
#define SIOD_GPIO_NUM   4
#define SIOC_GPIO_NUM   5
#define Y9_GPIO_NUM     16
#define Y8_GPIO_NUM     17
#define Y7_GPIO_NUM     18
#define Y6_GPIO_NUM     12
#define Y5_GPIO_NUM     10
#define Y4_GPIO_NUM     8
#define Y3_GPIO_NUM     9
#define Y2_GPIO_NUM     11
#define VSYNC_GPIO_NUM  6
#define HREF_GPIO_NUM   7
#define PCLK_GPIO_NUM   13

WiFiUDP udp;
QueueHandle_t frameQueue;
struct Frame { uint8_t *buf; size_t len; };

void cameraTask(void *arg) {
  uint32_t fps_count = 0, fps_time = millis();
  while (true) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) continue;

    fps_count++;
    if (millis() - fps_time >= 1000) {
      Serial.printf("CAM FPS: %d | %d байт\n", fps_count, fb->len);
      fps_count = 0; fps_time = millis();
    }

    if (fb->len > 20000) { esp_camera_fb_return(fb); continue; }

    Frame f;
    f.len = fb->len;
    f.buf = (uint8_t*)heap_caps_malloc(fb->len, MALLOC_CAP_SPIRAM);
    if (f.buf) {
      memcpy(f.buf, fb->buf, fb->len);
      Frame old;
      if (uxQueueMessagesWaiting(frameQueue) >= 2) {
        xQueueReceive(frameQueue, &old, 0);
        heap_caps_free(old.buf);
      }
      xQueueSend(frameQueue, &f, 0);
    }
    esp_camera_fb_return(fb);
  }
}

void networkTask(void *arg) {
  uint32_t net_fps = 0, net_time = millis();
  uint32_t frame_id = 0;

  while (true) {
    Frame f;
    if (!xQueueReceive(frameQueue, &f, pdMS_TO_TICKS(100))) continue;

    uint16_t total_chunks = (f.len + CHUNK_SIZE - 1) / CHUNK_SIZE;

    for (uint16_t i = 0; i < total_chunks; i++) {
      size_t offset    = i * CHUNK_SIZE;
      size_t chunk_len = min((size_t)CHUNK_SIZE, f.len - offset);

      uint8_t hdr[8];
      hdr[0] = (frame_id >> 24) & 0xFF;
      hdr[1] = (frame_id >> 16) & 0xFF;
      hdr[2] = (frame_id >>  8) & 0xFF;
      hdr[3] =  frame_id        & 0xFF;
      hdr[4] = (i >> 8) & 0xFF;
      hdr[5] =  i       & 0xFF;
      hdr[6] = (total_chunks >> 8) & 0xFF;
      hdr[7] =  total_chunks       & 0xFF;

      for (int retry = 0; retry < 3; retry++) {
        udp.beginPacket(PC_IP, PC_PORT);
        udp.write(hdr, 8);
        udp.write(f.buf + offset, chunk_len);
        if (udp.endPacket()) break;
        vTaskDelay(1 / portTICK_PERIOD_MS);
      }

      if (i % 4 == 3) vTaskDelay(1 / portTICK_PERIOD_MS);
    }

    heap_caps_free(f.buf);
    frame_id++;

    net_fps++;
    if (millis() - net_time >= 1000) {
      Serial.printf("NET FPS: %d\n", net_fps);
      net_fps = 0; net_time = millis();
    }
  }
}

void setup() {
  Serial.begin(115200);

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0; config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0=Y2_GPIO_NUM; config.pin_d1=Y3_GPIO_NUM;
  config.pin_d2=Y4_GPIO_NUM; config.pin_d3=Y5_GPIO_NUM;
  config.pin_d4=Y6_GPIO_NUM; config.pin_d5=Y7_GPIO_NUM;
  config.pin_d6=Y8_GPIO_NUM; config.pin_d7=Y9_GPIO_NUM;
  config.pin_xclk=XCLK_GPIO_NUM; config.pin_pclk=PCLK_GPIO_NUM;
  config.pin_vsync=VSYNC_GPIO_NUM; config.pin_href=HREF_GPIO_NUM;
  config.pin_sccb_sda=SIOD_GPIO_NUM; config.pin_sccb_scl=SIOC_GPIO_NUM;
  config.pin_pwdn=PWDN_GPIO_NUM; config.pin_reset=RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_VGA;
  config.jpeg_quality = 20;
  config.fb_count     = 3;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_LATEST;

  if (esp_camera_init(&config) != ESP_OK) {
    Serial.println("CAM FAIL"); return;
  }

  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 0);
  s->set_hmirror(s, 0);
  s->set_exposure_ctrl(s, 0);
  s->set_aec_value(s, 300);
  s->set_gain_ctrl(s, 0);
  s->set_agc_gain(s, 5);

  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) { delay(300); Serial.print("."); }
  Serial.println("\nIP: " + WiFi.localIP().toString());

  udp.begin(UDP_PORT);
  frameQueue = xQueueCreate(3, sizeof(Frame));
  xTaskCreatePinnedToCore(cameraTask,  "cam", 8192, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(networkTask, "net", 8192, NULL, 5, NULL, 1);
}

void loop() { vTaskDelay(1000 / portTICK_PERIOD_MS); }
