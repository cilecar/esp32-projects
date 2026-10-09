// ================================================================
//  Автономный видеорегистратор на ESP32-S3 N16R8 (Freenove-раскладка)
//  OV2640 (DVP) -> MJPEG на microSD, без WiFi.
//
//  Кольцевая запись: пишет бесконечно, по мере заполнения карты затирает
//  самые старые куски, поэтому последние TARGET_HOURS часов есть всегда.
//  Качество JPEG подстраивается само, чтобы эта глубина архива соблюдалась.
//
//  При старте на несколько секунд поднимается WiFi ради времени по NTP,
//  после чего радио выключается; дата попадает в имена файлов.
//
//  Пишет поток склеенных JPEG. Конвертация на ПК:
//    ffmpeg -f mjpeg -r 10 -i cam_00000_*.mjpeg out.mp4
//  ВАЖНО: -r ставить по реальному fps из строки статуса, а не по TARGET_FPS.
//  На XGA тракт держит номинальные 10; если поднять разрешение и сенсор начнёт
//  отставать, -r надо будет поправить, иначе запись пойдёт в ускоренном темпе.
//
//  --- Обязательные настройки Arduino IDE ---
//  Board .............. ESP32S3 Dev Module
//  PSRAM .............. OPI PSRAM      (иначе камера не инициализируется)
//  USB CDC On Boot .... Disabled       (иначе Serial уходит в нативный USB,
//                                       а не в UART-мост CH343 -> /dev/ttyACM0)
//  Flash Size ......... 16MB (128Mb)
//  Partition Scheme ... Huge APP (3MB No OTA/1MB SPIFFS)
//  Порт ............... разъём UART платы, /dev/ttyACM0
// ================================================================

#include "esp_camera.h"
#include "SD_MMC.h"
#include "FS.h"
#include <WiFi.h>
#include <esp_bt.h>
#include <time.h>
#include <esp_heap_caps.h>

// ---------------- Камера (распаяна на плате) ----------------
#define CAM_PIN_PWDN    -1
#define CAM_PIN_RESET   -1
#define CAM_PIN_XCLK    15
#define CAM_PIN_SIOD     4
#define CAM_PIN_SIOC     5
#define CAM_PIN_Y2      11
#define CAM_PIN_Y3       9
#define CAM_PIN_Y4       8
#define CAM_PIN_Y5      10
#define CAM_PIN_Y6      12
#define CAM_PIN_Y7      18
#define CAM_PIN_Y8      17
#define CAM_PIN_Y9      16
#define CAM_PIN_VSYNC    6
#define CAM_PIN_HREF     7
#define CAM_PIN_PCLK    13

// ---------------- SD-слот (SDMMC, 1-битный режим) ----------------
#define SD_CLK  39   // MTCK
#define SD_CMD  38
#define SD_D0   40   // MTDO

// Частота шины SD. Слот разведён однобитно (наружу идёт только D0), четырёхбитный
// режим недоступен физически — поэтому единственный рычаг скорости здесь частота.
// 40 МГц берут не все карты; если не заведётся, код сам откатится на штатные 20.
#define SD_FREQ_KHZ  40000

// ---------------- Часы: NTP при старте ----------------
// WiFi поднимается только чтобы узнать время, потом гасится совсем.
// Логин и пароль лежат в secrets.h — он в .gitignore и в репозиторий не уезжает.
// Нет файла (свежий клон) — собираемся без сети, файлы будут по номерам.
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #warning "Нет secrets.h — WiFi отключён. Скопируй secrets.h.example в secrets.h"
  #define WIFI_SSID ""
  #define WIFI_PASS ""
#endif

#define WIFI_TIMEOUT_MS 15000           // сколько ждать подключения
#define NTP_SERVER      "pool.ntp.org"
#define TZ_INFO         "MSK-3"         // Москва, UTC+3 (в POSIX знак обратный)

// ---------------- Индикация ----------------
#define LED_PIN          2   // бортовой светодиод "LED ON"; -1 = погасить совсем
#define LED_BLINK_ON_REC 0   // 1 = пульс раз в секунду во время записи
#define LED_BLINK_ON_ERR 1   // 1 = мигать при аварии (камера/SD/карта кончилась)

// ---------------- Настройки записи ----------------
#define TARGET_FPS      10               // кадров в секунду
#define FRAME_SIZE      FRAMESIZE_XGA    // 1024x768
#define JPEG_QUALITY    12               // 0..63, меньше = лучше и тяжелее (стартовое)

#define TARGET_HOURS      18             // глубина архива, которая обязана помещаться
#define QUALITY_BEST      6              // предел, лучше которого не поднимаемся
#define QUALITY_WORST     28             // предел, ниже которого не опускаемся
#define MIN_FPS           8.5f           // ниже этого качество не улучшаем: место
                                         // на карте есть, а вот кадры не восполнить

#define RING_MODE         1              // 1 = кольцо: затирать старое и писать вечно
                                         // 0 = остановиться, когда карта кончится
#define RING_FREE_MB      1500           // опустились ниже — удаляем самый старый файл

#define MINUTES_PER_FILE  10             // ротация файла (шаг, которым стареет архив)
#define FLUSH_EVERY_N     100            // сброс буфера на карту (защита от обесточивания);
                                         // каждый flush переписывает служебные записи FAT,
                                         // поэтому частить им дорого
#define MIN_FREE_MB       200            // неприкосновенный резерв на карте

#define FRAMES_PER_FILE  ((uint32_t)TARGET_FPS * 60 * MINUTES_PER_FILE)
#define FRAME_INTERVAL   (1000 / TARGET_FPS)
#define TARGET_SECONDS   ((uint32_t)TARGET_HOURS * 3600)

// ================================================================

static uint32_t fileIndex   = 0;   // индекс для следующего создаваемого файла
static uint32_t frameCount  = 0;
static bool     timeValid   = false;          // удалось ли узнать реальное время
static char     curFilePath[64] = "";         // файл под записью — его не удаляем

// Накопительный буфер записи. Решает сразу две проблемы:
//
// 1. Кадр камеры лежит в PSRAM, а SDMMC работает через DMA и внешнюю память
//    напрямую не переваривает — FATFS перекладывает её мелкими порциями.
// 2. Куда важнее: JPEG имеет произвольный размер, не кратный сектору в 512 Б.
//    После первого кадра позиция в файле уходит с границы сектора навсегда, а
//    FATFS пишет пачку секторов одной командой только при выровненной позиции.
//    Иначе он ползёт по сектору за раз — десятки обращений к карте на кадр.
//
// Поэтому копим во внутренней памяти и отдаём карте всегда ровно по WR_CHUNK,
// кратному сектору. Позиция в файле остаётся выровненной, хвост переезжает
// в следующую порцию. Незаписанным висит не больше одного кадра.
#define WR_CHUNK 32768                        // 32 КБ: кратно и сектору, и кластеру
static uint8_t *wrBuf = nullptr;
static size_t   wrLen = 0;                    // сколько уже накоплено

// Счётчики за последний отчётный интервал. Накопительное среднее с момента
// загрузки сглаживает и прячет текущее состояние, а нам нужно видеть именно его.
static uint64_t ivBytes   = 0;
static uint32_t ivFrames  = 0;
static uint32_t ivStartMs = 0;
static uint32_t ivCapUs   = 0;   // время в esp_camera_fb_get()
static uint32_t ivWrUs    = 0;   // время в videoFile.write()
static File     videoFile;
static bool     sdOk = false;

static uint64_t bytesWritten  = 0;             // сколько уже уложено на карту
static uint32_t recStartMs    = 0;             // момент старта записи
static int      curQuality    = JPEG_QUALITY;  // текущее качество, им рулит регулятор

// Останавливает работу; если разрешено — мигает светодиодом как кодом ошибки
static void haltWithBlink(uint16_t period_ms) {
    while (true) {
        if (LED_PIN >= 0 && LED_BLINK_ON_ERR) {
            digitalWrite(LED_PIN, HIGH);
            delay(period_ms);
            digitalWrite(LED_PIN, LOW);
        }
        delay(period_ms);
    }
}

// Поднимает WiFi, берёт время с NTP и сразу глушит радио.
// Дальше время идёт от кварца 40 МГц — дрейф порядка пары секунд в сутки,
// чего с запасом хватает, чтобы датировать записи.
static bool syncTimeOverWiFi() {
    if (strlen(WIFI_SSID) == 0) {
        Serial.println("Часы: сеть не задана, файлы будут по номерам");
        return false;
    }

    Serial.printf("Часы: подключаюсь к \"%s\"", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < WIFI_TIMEOUT_MS) {
        delay(250);
        Serial.print(".");
    }
    Serial.println();

    bool ok = false;
    if (WiFi.status() == WL_CONNECTED) {
        configTzTime(TZ_INFO, NTP_SERVER, "time.google.com");
        struct tm ti;
        ok = getLocalTime(&ti, 10000);
        if (ok) {
            char buf[32];
            strftime(buf, sizeof(buf), "%d.%m.%Y %H:%M:%S", &ti);
            Serial.printf("Часы: %s\n", buf);
        } else {
            Serial.println("Часы: NTP не ответил");
        }
    } else {
        Serial.println("Часы: сеть недоступна");
    }

    // Радио своё дело сделало — дальше оно только ест батарею
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    return ok;
}

static bool initCamera() {
    camera_config_t cfg = {};
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.ledc_timer   = LEDC_TIMER_0;
    cfg.pin_d0       = CAM_PIN_Y2;
    cfg.pin_d1       = CAM_PIN_Y3;
    cfg.pin_d2       = CAM_PIN_Y4;
    cfg.pin_d3       = CAM_PIN_Y5;
    cfg.pin_d4       = CAM_PIN_Y6;
    cfg.pin_d5       = CAM_PIN_Y7;
    cfg.pin_d6       = CAM_PIN_Y8;
    cfg.pin_d7       = CAM_PIN_Y9;
    cfg.pin_xclk     = CAM_PIN_XCLK;
    cfg.pin_pclk     = CAM_PIN_PCLK;
    cfg.pin_vsync    = CAM_PIN_VSYNC;
    cfg.pin_href     = CAM_PIN_HREF;
    cfg.pin_sccb_sda = CAM_PIN_SIOD;
    cfg.pin_sccb_scl = CAM_PIN_SIOC;
    cfg.pin_pwdn     = CAM_PIN_PWDN;
    cfg.pin_reset    = CAM_PIN_RESET;
    cfg.xclk_freq_hz = 20000000;
    cfg.pixel_format = PIXFORMAT_JPEG;
    cfg.frame_size   = FRAME_SIZE;
    cfg.jpeg_quality = JPEG_QUALITY;
    cfg.fb_count     = 2;                      // двойная буферизация в PSRAM
    cfg.grab_mode    = CAMERA_GRAB_LATEST;
    cfg.fb_location  = CAMERA_FB_IN_PSRAM;

    if (esp_camera_init(&cfg) != ESP_OK) return false;

    // Небольшая подстройка сенсора: OV2640 по умолчанию даёт перевёрнутую картинку
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_vflip(s, 1);
        s->set_hmirror(s, 0);
        s->set_brightness(s, 0);
        s->set_saturation(s, 0);
    }
    return true;
}

static bool initSD() {
    SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);

    // mode1bit = true, format_if_empty = false, дальше частота шины
    if (SD_MMC.begin("/sdcard", true, false, SD_FREQ_KHZ)) {
        Serial.printf("SD: шина %d кГц\n", SD_FREQ_KHZ);
        return true;
    }

    SD_MMC.end();                       // не потянула разгон — откатываемся
    if (SD_MMC.begin("/sdcard", true, false)) {
        Serial.println("SD: разгон не пошёл, работаем на штатных 20 МГц");
        return true;
    }
    return false;
}

// Дописывает накопленный хвост как есть. Выравнивание тут уже нарушается,
// но это происходит один раз на файл — перед его закрытием.
static void flushTail() {
    if (wrBuf && wrLen) {
        videoFile.write(wrBuf, wrLen);
        wrLen = 0;
    }
}

// Копит кадр в буфере и отдаёт карте целыми порциями по WR_CHUNK.
// Если буфер выделить не удалось — откатываемся на прямую запись из PSRAM.
static size_t writeFrame(const uint8_t *src, size_t len) {
    if (!wrBuf) return videoFile.write(src, len);

    size_t done = 0;
    while (done < len) {
        size_t space = WR_CHUNK - wrLen;
        size_t n     = len - done;
        if (n > space) n = space;

        memcpy(wrBuf + wrLen, src + done, n);
        wrLen += n;
        done  += n;

        if (wrLen == WR_CHUNK) {              // порция набралась — отдаём целиком
            size_t w = videoFile.write(wrBuf, WR_CHUNK);
            wrLen = 0;
            if (w != WR_CHUNK) return done - n;   // недопись — сигнал наверх
        }
    }
    return done;
}

static uint64_t freeMB() {
    return (SD_MMC.totalBytes() - SD_MMC.usedBytes()) / (1024ULL * 1024ULL);
}

// Обходит каталог и находит границы архива: куда писать дальше и что удалять первым.
// Простым «первым свободным индексом» тут не обойтись — в кольцевом режиме начало
// нумерации выедено удалениями, и запись ушла бы в дыру между старыми файлами.
static void scanExistingFiles() {
    uint32_t minIdx = UINT32_MAX, maxIdx = 0;
    bool     found  = false;

    File root = SD_MMC.open("/");
    if (root) {
        File f;
        while ((f = root.openNextFile())) {
            // name() в разных версиях ядра отдаёт то путь, то голое имя
            const char   *tag = strstr(f.name(), "cam_");
            unsigned long idx;
            // расширение проверяем отдельно: sscanf не считает несовпадение
            // хвостового литерала ошибкой и принял бы любой cam_123.*
            if (tag && strstr(tag, ".mjpeg") && sscanf(tag, "cam_%lu", &idx) == 1) {
                if (idx < minIdx) minIdx = idx;
                if (idx > maxIdx) maxIdx = idx;
                found = true;
            }
            f.close();
        }
        root.close();
    }

    fileIndex = found ? maxIdx + 1 : 0;

    if (found) {
        Serial.printf("Найден архив: файлы %lu..%lu\n",
                      (unsigned long)minIdx, (unsigned long)maxIdx);
    }
}

// Находит файл с наименьшим номером — самый старый кусок архива.
// Ищем именно обходом каталога, а не подстановкой номера в шаблон: с датой
// в имени восстановить его по одному лишь индексу уже нельзя.
static bool findOldestFile(char *out, size_t len) {
    uint32_t bestIdx = UINT32_MAX;
    bool     found   = false;

    File root = SD_MMC.open("/");
    if (!root) return false;

    File f;
    while ((f = root.openNextFile())) {
        const char   *tag = strstr(f.name(), "cam_");
        unsigned long idx;
        if (tag && strstr(tag, ".mjpeg") && sscanf(tag, "cam_%lu", &idx) == 1 && idx < bestIdx) {
            char cand[64];
            snprintf(cand, sizeof(cand), "/%s", tag);   // name() бывает и путём, и голым именем
            if (strcmp(cand, curFilePath) != 0) {       // тот, что пишется сейчас, не трогаем
                bestIdx = idx;
                snprintf(out, len, "%s", cand);
                found = true;
            }
        }
        f.close();
    }
    root.close();
    return found;
}

// Удаляет самый старый файл архива. Текущий пишущийся не трогает никогда.
static bool deleteOldest() {
    char path[64];
    if (!findOldestFile(path, sizeof(path))) return false;   // архив кончился

    if (SD_MMC.remove(path)) {
        Serial.printf("   удалён старый %s\n", path);
        return true;
    }
    Serial.printf("   НЕ удалось удалить %s\n", path);
    return false;
}

// Освобождает место под продолжение записи, срезая архив с хвоста
static void ensureFreeSpace() {
    int guard = 0;
    while (freeMB() < RING_FREE_MB && guard++ < 20) {
        if (!deleteOldest()) break;
    }
}

// Имя вида cam_00144_20260813_223015.mjpeg: номер спереди задаёт порядок,
// по нему работает кольцо; дата сзади — чтобы глазами найти нужный момент.
// Без синхронизации времени остаётся только номер.
static void makeFileName(char *out, size_t len, uint32_t idx) {
    struct tm ti;
    if (timeValid && getLocalTime(&ti, 10)) {
        char ts[20];
        strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", &ti);
        snprintf(out, len, "/cam_%05lu_%s.mjpeg", (unsigned long)idx, ts);
    } else {
        snprintf(out, len, "/cam_%05lu.mjpeg", (unsigned long)idx);
    }
}

static bool openNewFile() {
    if (videoFile) {
        flushTail();                // хвост принадлежит уходящему файлу
        videoFile.close();
    }

    makeFileName(curFilePath, sizeof(curFilePath), fileIndex++);
    videoFile = SD_MMC.open(curFilePath, FILE_WRITE);
    if (!videoFile) {
        Serial.printf("Не удалось создать %s\n", curFilePath);
        curFilePath[0] = '\0';
        return false;
    }
    Serial.printf("Пишу в %s\n", curFilePath);
    return true;
}

// Держит запись в графике: размер JPEG сильно зависит от сцены, поэтому
// сравниваем фактический расход с тем, что можно себе позволить до TARGET_HOURS,
// и двигаем качество на один шаг. Шаг раз в минуту — регулятор заведомо не звенит.
static void adjustQuality(uint32_t actualRate, float realFps) {
    uint32_t elapsed = (millis() - recStartMs) / 1000;
    if (elapsed < 60) return;                       // рано, статистики нет

    uint32_t allowedRate;

#if RING_MODE
    // Кольцо: под архив отдана вся карта минус рабочий зазор, и в неё должны
    // укладываться TARGET_HOURS. Цель постоянная — со временем не ужесточается.
    uint64_t capacity = SD_MMC.totalBytes();
    uint64_t reserve  = (uint64_t)RING_FREE_MB * 1024 * 1024;
    capacity = (capacity > reserve) ? (capacity - reserve) : 0;
    allowedRate = (uint32_t)(capacity / TARGET_SECONDS);
#else
    // Одноразовый прогон: остаток места делим на оставшееся до цели время
    if (elapsed >= TARGET_SECONDS) return;          // цель взята, дальше не вмешиваемся
    uint32_t remaining = TARGET_SECONDS - elapsed;

    uint64_t freeBytes = SD_MMC.totalBytes() - SD_MMC.usedBytes();
    uint64_t reserve   = (uint64_t)MIN_FREE_MB * 1024 * 1024;
    freeBytes = (freeBytes > reserve) ? (freeBytes - reserve) : 0;

    allowedRate = (uint32_t)(freeBytes / remaining);
#endif

    // Регулятор следит за двумя вещами сразу, и частота кадров важнее места:
    // место кольцо освободит само, а вычеркнутые из секунды кадры не вернуть.
    int newQuality = curQuality;
    if (realFps < MIN_FPS) {
        // Карта не успевает принимать поток. Утяжелять кадр здесь бесполезно —
        // байт в секунду от этого не прибавится, просто кадров пройдёт меньше.
        // Единственный рычаг обратно — облегчить кадр.
        newQuality++;
    } else if (actualRate > (allowedRate / 100) * 95) {
        newQuality++;                               // не укладываемся в место
    } else if (actualRate < (allowedRate / 100) * 65 && realFps > MIN_FPS * 1.1f) {
        // Улучшаем только с явным запасом по частоте, иначе регулятор начнёт
        // звенеть между двумя соседними ступенями вокруг самого порога
        newQuality--;
    }

    if (newQuality < QUALITY_BEST)  newQuality = QUALITY_BEST;
    if (newQuality > QUALITY_WORST) newQuality = QUALITY_WORST;

    if (newQuality != curQuality) {
        sensor_t *s = esp_camera_sensor_get();
        if (s && s->set_quality(s, newQuality) == 0) {
            Serial.printf("   качество %d -> %d (расход %lu КБ/с, лимит %lu КБ/с)\n",
                          curQuality, newQuality,
                          (unsigned long)(actualRate / 1024),
                          (unsigned long)(allowedRate / 1024));
            curQuality = newQuality;
        }
    }
}

void setup() {
    Serial.begin(115200);
    if (LED_PIN >= 0) {
        pinMode(LED_PIN, OUTPUT);
        digitalWrite(LED_PIN, LOW);
    }
    delay(300);

    btStop();                        // Bluetooth не нужен вовсе

    Serial.println("\n=== ESP32-S3 автономный регистратор ===");

    // WiFi живёт только эти несколько секунд, дальше гаснет внутри функции
    timeValid = syncTimeOverWiFi();

    if (!initCamera()) {
        Serial.println("ОШИБКА: камера не поднялась (проверь шлейф в разъёме)");
        haltWithBlink(100);          // частое мигание = камера
    }
    Serial.println("Камера: OK");

    // Буфер должен лежать во внутренней памяти: ради этого он и заводится
    wrBuf = (uint8_t *)heap_caps_malloc(WR_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    Serial.printf("Буфер записи: %s (внутренней памяти свободно %lu Б)\n",
                  wrBuf ? "32 КБ во внутренней памяти" : "НЕ ВЫДЕЛЕН, пишем из PSRAM",
                  (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));

    if (!initSD()) {
        Serial.println("ОШИБКА: SD не читается (вставлена? FAT32?)");
        haltWithBlink(500);          // редкое мигание = SD
    }

    Serial.printf("SD: %llu МБ всего, %llu МБ свободно\n",
                  SD_MMC.totalBytes() / (1024ULL * 1024ULL), freeMB());
    // ~700 КБ/с при XGA / 10 fps / quality 12 — дальше уточнится по факту
#if RING_MODE
    uint64_t archiveMB = SD_MMC.totalBytes() / (1024ULL * 1024ULL) - RING_FREE_MB;
    Serial.printf("Кольцо: глубина архива ~%llu ч, гарантируем %d ч\n",
                  (archiveMB * 1024ULL) / (700ULL * 3600ULL), TARGET_HOURS);
#else
    Serial.printf("Оценка: ~%llu ч записи, цель %d ч\n",
                  (freeMB() * 1024ULL) / (700ULL * 3600ULL), TARGET_HOURS);
#endif

    scanExistingFiles();
#if RING_MODE
    ensureFreeSpace();          // карта могла остаться заполненной с прошлого раза
#endif
    if (!openNewFile()) haltWithBlink(500);

    sdOk = true;
    recStartMs = ivStartMs = millis();
    Serial.printf("Старт: XGA 1024x768, %d fps, quality %d\n", TARGET_FPS, curQuality);
}

void loop() {
    // Карта отвалилась — пробуем поднять её заново раз в секунду
    if (!sdOk) {
        if (LED_PIN >= 0) digitalWrite(LED_PIN, LOW);
        SD_MMC.end();
        delay(1000);
        if (initSD()) {
            scanExistingFiles();
            sdOk = openNewFile();
            Serial.println("SD восстановлена");
        }
        return;
    }

    uint32_t frameStart = millis();

    uint32_t tCap0 = micros();
    camera_fb_t *fb = esp_camera_fb_get();
    uint32_t tCap1 = micros();
    if (!fb) {
        Serial.println("Кадр не получен");
        delay(FRAME_INTERVAL);
        return;
    }

    size_t frameLen = fb->len;                        // читаем до возврата буфера
    size_t written  = writeFrame(fb->buf, frameLen);
    uint32_t tWr1   = micros();
    esp_camera_fb_return(fb);

    ivCapUs += tCap1 - tCap0;
    ivWrUs  += tWr1  - tCap1;

    if (written != frameLen) {
        Serial.println("Сбой записи — переинициализация SD");
        wrLen = 0;                  // недописанный хвост уже не спасти
        videoFile.close();
        sdOk = false;
        return;
    }

    bytesWritten += written;
    frameCount++;
    ivBytes += written;
    ivFrames++;

    // Периодический flush: при внезапном отключении теряем максимум 3 секунды
    if (frameCount % FLUSH_EVERY_N == 0) videoFile.flush();

#if LED_BLINK_ON_REC
    // Короткая вспышка раз в секунду — признак живой записи
    if (LED_PIN >= 0) digitalWrite(LED_PIN, (frameCount % TARGET_FPS == 0) ? HIGH : LOW);
#endif

    if (frameCount % FRAMES_PER_FILE == 0) {
#if RING_MODE
        ensureFreeSpace();      // расчищаем место до того, как начнём новый кусок
#endif
        if (!openNewFile()) { sdOk = false; return; }
    }

    // Раз в минуту: статус, регулятор качества и работа с местом на карте
    if (frameCount % ((uint32_t)TARGET_FPS * 60) == 0) {
        uint32_t elapsed = (millis() - recStartMs) / 1000;

        // Всё считаем за прошедший интервал, а не с момента загрузки.
        // Время берём в миллисекундах: округление до целых секунд заметно
        // завышало fps, а по нему потом выставляется -r при склейке.
        uint32_t ivMs     = millis() - ivStartMs;
        if (ivMs == 0) ivMs = 1;
        uint32_t rateB    = (uint32_t)(ivBytes * 1000ULL / ivMs);
        uint32_t rateKBs  = rateB / 1024;
        float    realFps  = ivFrames * 1000.0f / ivMs;
        // Куда уходит время кадра — это и показывает, что именно упирается
        uint32_t capMs    = ivFrames ? ivCapUs / ivFrames / 1000 : 0;
        uint32_t wrMs     = ivFrames ? ivWrUs  / ivFrames / 1000 : 0;

#if RING_MODE
        // Глубина архива — сколько часов прошлого сейчас лежит на карте
        uint64_t archMB = SD_MMC.usedBytes() / (1024ULL * 1024ULL);
        uint32_t depthH = rateKBs ? (uint32_t)((archMB * 1024) / (rateKBs * 3600ULL)) : 0;

        Serial.printf("[%lu мин] %.1f fps, %lu КБ/с, q%d | кадр %lu мс, запись %lu мс | архив %llu МБ ~%lu ч\n",
                      (unsigned long)(elapsed / 60), realFps, (unsigned long)rateKBs,
                      curQuality, (unsigned long)capMs, (unsigned long)wrMs,
                      archMB, (unsigned long)depthH);

        adjustQuality(rateB, realFps);
        ensureFreeSpace();
#else
        uint64_t left      = freeMB();
        uint32_t hoursLeft = rateKBs ? (uint32_t)((left * 1024) / (rateKBs * 3600ULL)) : 0;

        Serial.printf("[%lu мин] %.1f fps, %lu КБ/с, q%d | кадр %lu мс, запись %lu мс | свободно %llu МБ ~%lu ч\n",
                      (unsigned long)(elapsed / 60), realFps, (unsigned long)rateKBs,
                      curQuality, (unsigned long)capMs, (unsigned long)wrMs,
                      left, (unsigned long)hoursLeft);

        adjustQuality(rateB, realFps);

        if (left < MIN_FREE_MB) {
            flushTail();
            videoFile.flush();
            videoFile.close();
            Serial.println("Карта заполнена — запись остановлена");
            haltWithBlink(1000);
        }
#endif

        // Обнуляем интервальные счётчики — следующий отчёт снова про «сейчас»
        ivBytes = ivFrames = ivCapUs = ivWrUs = 0;
        ivStartMs = millis();
    }

    int32_t elapsed = (int32_t)(millis() - frameStart);
    int32_t wait    = FRAME_INTERVAL - elapsed;
    if (wait > 0) delay(wait);
}
