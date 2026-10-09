#pragma once

// WIFI_SSID, WIFI_PASSWORD, NTFY_TOPIC, TARGET_MAC и OTA_PASSWORD — в secrets.h
#include "secrets.h"

// IP-адрес ПК в локальной сети (лучше сделать статическим в роутере)
// Узнать: ip addr | grep "inet "
#define TARGET_IP 192, 168, 31, 31

// Интервал проверки статуса ПК (пинг) в миллисекундах
#define STATUS_CHECK_INTERVAL_MS 30000

