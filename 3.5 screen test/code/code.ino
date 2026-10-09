#include "esp_camera.h"
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX_ESP32S3_ILI9488 : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9488 _panel_instance;
  lgfx::Bus_SPI       _bus_instance;
  lgfx::Touch_XPT2046 _touch_instance;

public:
  LGFX_ESP32S3_ILI9488(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read  = 16000000;
      cfg.pin_sclk = 42;
      cfg.pin_mosi = 1;
      cfg.pin_miso = 2;
      cfg.pin_dc   = 21;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs           = 41;
      cfg.pin_rst          = 47;
      cfg.panel_width      = 320;
      cfg.panel_height     = 480;
      cfg.offset_x         = 0;
      cfg.offset_y         = 0;
      _panel_instance.config(cfg);
    }
    {
      auto cfg = _touch_instance.config();
      cfg.x_min      = 0;
      cfg.x_max      = 319;
      cfg.y_min      = 0;
      cfg.y_max      = 479;
      cfg.pin_cs     = 14;
      cfg.pin_int    = -1;
      cfg.freq       = 2500000;
      cfg.bus_shared = true;
      cfg.spi_host   = SPI2_HOST;
      _touch_instance.config(cfg);
      _panel_instance.setTouch(&_touch_instance);
    }
    setPanel(&_panel_instance);
  }
};

LGFX_ESP32S3_ILI9488 lcd;

enum ScreenState { MAIN_MENU, CAMERA_STREAM, INFO_PAGE };
ScreenState currentState = MAIN_MENU;

#define PWDN_GPIO_NUM -1
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 15
#define SIOD_GPIO_NUM 4
#define SIOC_GPIO_NUM 5
#define Y9_GPIO_NUM 16
#define Y8_GPIO_NUM 17
#define Y7_GPIO_NUM 18
#define Y6_GPIO_NUM 12
#define Y5_GPIO_NUM 10
#define Y4_GPIO_NUM 8
#define Y3_GPIO_NUM 9
#define Y2_GPIO_NUM 11
#define VSYNC_GPIO_NUM 6
#define HREF_GPIO_NUM 7
#define PCLK_GPIO_NUM 13

bool isPressed(int tx, int ty, int x, int y, int w, int h) {
  return (tx >= x && tx <= x + w && ty >= y && ty <= y + h);
}

void drawMenu() {
  lcd.fillScreen(0x2104);
  lcd.setFont(&fonts::efontJA_24);
  
  lcd.fillRoundRect(40, 60, 400, 80, 10, TFT_BLUE);
  lcd.drawRoundRect(40, 60, 400, 80, 10, TFT_WHITE);
  lcd.setTextColor(TFT_WHITE);
  lcd.drawCenterString("КАМЕРА", 240, 85);

  lcd.fillRoundRect(40, 160, 400, 80, 10, 0x03E0);
  lcd.drawRoundRect(40, 160, 400, 80, 10, TFT_WHITE);
  lcd.drawCenterString("ИНФО", 240, 185);
}

void drawInfo() {
  lcd.fillScreen(TFT_BLACK);
  lcd.setTextColor(TFT_GOLD);
  lcd.setFont(&fonts::efontJA_24);
  lcd.drawCenterString("ESP32-S3 + ILI9488", 240, 50);
  
  lcd.setFont(&fonts::efontJA_16);
  lcd.setCursor(20, 120);
  lcd.println("PSRAM: OK");
  lcd.println("Touch: XPT2046");
  
  lcd.fillRoundRect(140, 240, 200, 50, 5, TFT_RED);
  lcd.setTextColor(TFT_WHITE);
  lcd.drawCenterString("НАЗАД", 240, 255);
}

void setup() {
  lcd.init();
  lcd.setRotation(1);
  lcd.setSwapBytes(false);
  
  uint16_t calData[] = {306, 3833, 293, 222, 3907, 3877, 3823, 255};
  lcd.setTouchCalibrate(calData);

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_RGB565;
  config.frame_size = FRAMESIZE_HVGA;
  config.jpeg_quality = 10;
  config.fb_count = 1;
  
  esp_camera_init(&config);
  
  drawMenu();
}

void loop() {
  uint16_t tx, ty;
  bool touched = lcd.getTouch(&tx, &ty);

  switch (currentState) {
    case MAIN_MENU:
      if (touched) {
        if (isPressed(tx, ty, 40, 60, 400, 80)) {
          currentState = CAMERA_STREAM;
          lcd.fillScreen(TFT_BLACK);
          delay(200);
        } 
        else if (isPressed(tx, ty, 40, 160, 400, 80)) {
          currentState = INFO_PAGE;
          drawInfo();
          delay(200);
        }
      }
      break;

    case CAMERA_STREAM:
      {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
          lcd.pushImage(0, 0, fb->width, fb->height, (uint16_t *)fb->buf);
          esp_camera_fb_return(fb);
        }
        if (touched) {
           currentState = MAIN_MENU;
           drawMenu();
           delay(200);
        }
      }
      break;

    case INFO_PAGE:
      if (touched && isPressed(tx, ty, 140, 240, 200, 50)) {
        currentState = MAIN_MENU;
        drawMenu();
        delay(200);
      }
      break;
  }
}
