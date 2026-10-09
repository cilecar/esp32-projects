# ESP32 Projects

Мои проекты на ESP32 / ESP32-S3 / ESP32-C3: умный дом, камеры, дисплеи, датчики и мелкие
эксперименты. Большинство скетчей написаны для Arduino IDE, `home secure` для MicroPython.
Рядом со многими проектами лежит схема подключения в Fritzing (`*.fzz`).

## Проекты

### Умный дом и автоматизация

| Проект | Что делает | Железо |
|---|---|---|
| [nighttime automation](nighttime%20automation) | Хаб с сенсорным экраном (LVGL) управляет светом в комнатах по ESP-NOW, будит ПК через Wake-on-LAN и усыпляет его через `sleep_server.py` | ESP32-S3 + дисплей, ESP32-C3, Sonoff |
| [MP-esp](MP-esp) | Удалённое включение ПК через Wake-on-LAN по командам из [ntfy.sh](https://ntfy.sh). Агент `pc_agent.py` на ПК выполняет команды (статус, скриншот, выключение) | ESP32 |
| [home secure](home%20secure) | Охранная система: PIR + ультразвук + BME280, уведомления в Telegram. Версия `mini secure` использует бота-прокси на Amvera | ESP32 (MicroPython / Arduino) |
| [balcony lighting](balcony%20lighting) | Освещение на балконе: реле + ШИМ с плавным включением, кнопка с антидребезгом, watchdog | ESP32-C3 |
| [plantation](plantation) | Фитолампа для кошачьей травы: «рассвет» и «закат» по NTP, таймлапс на SD, анализ роста по доле зелёного в кадре, веб-страница | ESP32-S3 CAM |
| [servo controll](servo%20controll) | Сервопривод управляется по датчику освещённости (LDR), кнопками и энкодером | ESP32 |

### Камеры и зрение

| Проект | Что делает | Железо |
|---|---|---|
| [ai-helper](ai-helper) | Снимает фото по кнопке, прокси `proxy.py` отправляет его в Gemini, ответ выводится на OLED | ESP32-S3 + OV2640 + SSD1306 |
| [cam](cam) | Автономный видеорегистратор: кольцевая запись MJPEG на microSD | ESP32-S3 + OV2640 |
| [s3camweb](s3camweb) | Стримит видео по UDP на ПК, где `main.py` собирает кадры и показывает их через OpenCV | ESP32-S3 CAM |
| [photo-s3](photo-s3) | Фото по срабатыванию PIR-датчика, просмотр через веб-сервер | ESP32-S3 CAM + AM312 |
| [3.5 screen test](3.5%20screen%20test) | Тест дисплея ILI9488 3.5" с тачем XPT2046 через LovyanGFX | ESP32-S3 |

### Дисплеи и гаджеты

| Проект | Что делает | Железо |
|---|---|---|
| [BLE + dashboard](BLE%20%2B%20dashboard) | Дашборд ПК на OLED (CPU, GPU, погода, музыка из `server.py`) + BLE-клавиатура для ввода паролей с SD. `3in1`: всё вместе плюс Wake-on-LAN и веб-файловый менеджер SD | ESP32 + OLED + энкодер + SD |
| [dashboard](dashboard) | Только дашборд ПК на OLED | ESP32 + SSD1306 |
| [pc-card unlocker](pc-card%20unlocker) | Разблокировка ПК RFID-картой: ESP32 притворяется BLE-клавиатурой и вводит пароль | ESP32 + MFRC522 |
| [network checher](network%20checher) | График пинга на OLED, данные шлёт `ping_tracker.py` с ПК | ESP32 + SSD1306 |
| [radar](radar) | «Радар»: ультразвуковой дальномер на сервоприводе + PIR, картинка на OLED | ESP32 |
| [safe](safe) | Кодовый замок: код набирается энкодером, засов на сервоприводе | ESP32 + OLED |
| [text reader](text%20reader) | Читалка текстов с SD на OLED, прокрутка энкодером | ESP32 + SSD1306 + SD |
| [video player](video%20player) | Видео на OLED 128×64 с SD. `convert.py` перегоняет mp4 в 1-битные кадры | ESP32 + SSD1306 + SD |
| [player](player) | Захват звука с I2S-микрофона | ESP32 + I2S-микрофон |
| [weather-bme](weather-bme) | Температура и давление с BMP280 | ESP32 + BMP280 |

В корне лежат картинки с распиновкой плат ESP32 DevKit v1, ESP32-C3 и ESP32-S3.

## Секреты

Пароли Wi-Fi, токены и прочие ключи в репозиторий не входят. В проектах, где они нужны,
рядом лежит шаблон. Скопируй его и впиши свои значения:

```sh
cp secrets.h.example secrets.h                 # Arduino-скетчи
cp credentials.py.example credentials.py       # home secure (MicroPython)
```

Кроме того:
- `MP-esp/pc_agent.py` берёт топик ntfy из переменной `NTFY_TOPIC` или из файла `MP-esp/.ntfy_topic`
- `ai-helper/proxy.py` берёт ключ из `GEMINI_API_KEY` или из файла `ai-helper/.gemini_key`

## Сборка

Arduino IDE или `arduino-cli` с ядром **esp32 by Espressif**. Нужные библиотеки указаны
в `#include` каждого скетча, рабочие настройки платы для некоторых проектов записаны
в комментарии в начале файла.

```sh
arduino-cli compile -b esp32:esp32:esp32s3 <папка-скетча>
```

> Некоторые старые скетчи (`3in1`, `pc-card unlocker`, `nighttime automation/hub`) писались под
> ядро ESP32 2.x и на 3.x не собираются: изменились API ESP-NOW и mbedtls, а библиотека
> BLE Keyboard несовместима с новым ядром.
