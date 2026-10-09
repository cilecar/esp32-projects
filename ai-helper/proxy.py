#!/usr/bin/env python3
"""
Прокси между ESP32-CAM и Gemini.

ESP32 шлёт сырой JPEG в теле POST /describe и получает обратно plain-text ответ,
уже причёсанный под дисплей. Ключ API живёт здесь, а не в прошивке.

    pip install flask requests
    export GEMINI_API_KEY=...
    python3 proxy.py

Посмотреть, какие модели доступны вашему ключу:
    python3 proxy.py --list
"""
import base64
import io
import os
import re
import sys
import time

import requests
from flask import Flask, Response, request

_KEY_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".gemini_key")
API_KEY = os.environ.get("GEMINI_API_KEY") or (open(_KEY_FILE).read().strip() if os.path.exists(_KEY_FILE) else "")
# Алиасы *-latest не привязаны к номеру версии и не протухают при обновлении моделей.
MODEL = os.environ.get("GEMINI_MODEL", "gemini-flash-lite-latest")
BASE = "https://generativelanguage.googleapis.com/v1beta"

_HERE = os.path.dirname(os.path.abspath(__file__))
LAST_FRAME = os.path.join(_HERE, "last_frame.jpg")      # как пришло с камеры
LAST_SENT = os.path.join(_HERE, "last_sent.jpg")        # что реально ушло в Gemini

# Поворот кадра по часовой стрелке, градусы: 0 / 90 / 180 / 270.
# ESP32 стоит горизонтально портами влево, сенсор при этом отдаёт кадр,
# повёрнутый на 90° против часовой, — компенсируем поворотом на 90 по часовой.
ROTATE_CW = int(os.environ.get("ROTATE_CW", "90"))

# Зеркалить по горизонтали. Применяется уже ПОСЛЕ поворота, то есть в координатах
# итоговой картинки — так проще подбирать на глаз. На сенсоре это делать нельзя:
# из-за поворота на 90° hmirror превратился бы в отражение по вертикали.
MIRROR = os.environ.get("MIRROR", "1") != "0"

# Вытягивание яркости. OV2640 в помещении стабильно недоэкспонирует, и Gemini
# в ответ пишет "кадр тёмный" вместо описания. Дешевле поправить здесь, чем
# гонять сенсор на пределе усиления (усиление добавляет шум, а не детали).
AUTOLEVEL = os.environ.get("AUTOLEVEL", "1") != "0"

# На дисплее буфер прокрутки 32 строки по ~24 символа. Берём с запасом на широкие буквы.
MAX_CHARS = 400

PROMPT = (
    "Опиши, что изображено на фотографии. Ответь по-русски, 2-3 предложения, "
    "не длиннее 350 символов. Без вводных фраз вроде «На изображении», "
    "без markdown и без переносов строк. "
    "Кадр снят дешёвой камерой: шум, смазанность и приглушённые цвета — норма, "
    "не описывай их, описывай содержимое. "
    "Только если разобрать действительно нечего — скажи об этом одной фразой."
)

app = Flask(__name__)
app.config["MAX_CONTENT_LENGTH"] = 4 * 1024 * 1024


def list_models() -> None:
    r = requests.get(f"{BASE}/models", headers={"x-goog-api-key": API_KEY}, timeout=30)
    r.raise_for_status()
    for m in r.json().get("models", []):
        if "generateContent" in m.get("supportedGenerationMethods", []):
            print(m["name"].removeprefix("models/"))


def extract_text(data: dict) -> str:
    """Достаёт текст ответа, отбрасывая thinking-части и обрабатывая блокировки."""
    candidates = data.get("candidates") or []
    if not candidates:
        reason = (data.get("promptFeedback") or {}).get("blockReason")
        return f"Запрос заблокирован: {reason}" if reason else "Пустой ответ модели"

    cand = candidates[0]
    parts = (cand.get("content") or {}).get("parts") or []
    chunks = [p["text"] for p in parts if "text" in p and not p.get("thought")]
    text = " ".join(chunks).strip()

    if not text and cand.get("finishReason") == "MAX_TOKENS":
        return "Ответ обрезан по лимиту токенов"
    return text


def tidy(text: str) -> str:
    """Одна строка, без разметки, обрезка по границе слова."""
    text = re.sub(r"[*_`#]", "", text)
    text = re.sub(r"\s+", " ", text).strip()
    if len(text) > MAX_CHARS:
        cut = text[:MAX_CHARS]
        space = cut.rfind(" ")
        text = (cut[:space] if space > MAX_CHARS * 0.6 else cut).rstrip(" ,.;:") + "..."
    return text


def plain(text: str) -> Response:
    return Response(text, mimetype="text/plain; charset=utf-8")


def preprocess(jpeg: bytes) -> bytes:
    """Поворот и вытягивание яркости. При любой ошибке отдаём исходник."""
    try:
        from PIL import Image, ImageOps
    except ImportError:
        return jpeg

    try:
        im = Image.open(io.BytesIO(jpeg))
        im.load()
        im = im.convert("RGB")

        if ROTATE_CW % 360:
            # PIL крутит против часовой, поэтому знак минус.
            im = im.rotate(-ROTATE_CW, expand=True)

        if MIRROR:
            im = ImageOps.mirror(im)

        if AUTOLEVEL:
            grey = im.convert("L")
            hist = grey.histogram()
            total = sum(hist) or 1
            mean = sum(i * hist[i] for i in range(256)) / total
            # Гамма тянем только когда кадр реально тёмный: на нормальном свете
            # она бы только выбелила картинку.
            if mean < 90:
                gamma = 0.55 if mean < 40 else 0.75
                lut = [min(255, int((v / 255.0) ** gamma * 255 + 0.5)) for v in range(256)]
                im = im.point(lut * 3)
            im = ImageOps.autocontrast(im, cutoff=(1, 1))

        out = io.BytesIO()
        im.save(out, format="JPEG", quality=88)
        return out.getvalue()
    except Exception as e:  # битый JPEG, нехватка памяти и т.п.
        print("препроцессинг пропущен:", e, flush=True)
        return jpeg


@app.post("/describe")
def describe():
    jpeg = request.get_data()
    if len(jpeg) < 1000:
        return plain("Кадр пустой")

    print(f"-> кадр {len(jpeg)} байт", flush=True)

    # Последний кадр всегда лежит рядом со скриптом — удобно посмотреть глазами,
    # что реально видит камера (резкость, ориентация, экспозиция).
    try:
        with open(LAST_FRAME, "wb") as f:
            f.write(jpeg)
    except OSError as e:
        print("не сохранил кадр:", e, flush=True)

    jpeg = preprocess(jpeg)
    try:
        with open(LAST_SENT, "wb") as f:
            f.write(jpeg)
    except OSError:
        pass

    payload = {
        "contents": [
            {
                "parts": [
                    {"text": PROMPT},
                    {
                        "inline_data": {
                            "mime_type": "image/jpeg",
                            "data": base64.b64encode(jpeg).decode(),
                        }
                    },
                ]
            }
        ],
        "generationConfig": {
            "temperature": 0.4,
            "maxOutputTokens": 400,
            # thinkingConfig сюда не кладём: gemini-3.5-flash-lite отвергает
            # thinkingBudget (400 INVALID_ARGUMENT) и по умолчанию рассуждения
            # всё равно не тратит. Если возьмёте "думающую" модель и получите
            # пустой текст при finishReason=MAX_TOKENS — добавьте
            # "thinkingConfig": {"thinkingLevel": "low"} и поднимите maxOutputTokens.
        },
    }

    # 500/503/504 у Gemini бывают спорадически ("Deadline expired") — это не
    # повод показывать ошибку на дисплее, повторяем.
    # Бюджет держим коротким: обычный ответ приходит за 1-3 с, и лучше быстро
    # отдать ошибку, чем заставить плату ждать полторы минуты.
    r = None
    for attempt in range(2):
        t0 = time.time()
        try:
            r = requests.post(
                f"{BASE}/models/{MODEL}:generateContent",
                headers={"x-goog-api-key": API_KEY, "Content-Type": "application/json"},
                json=payload,
                timeout=20,
            )
        except requests.RequestException as e:
            print(f"сеть (попытка {attempt + 1}), {time.time() - t0:.1f}s: {e}", flush=True)
            time.sleep(1.0)
            continue

        print(f"   Gemini {r.status_code} за {time.time() - t0:.1f} с", flush=True)
        if r.status_code not in (500, 503, 504):
            break
        time.sleep(1.0)

    if r is None:
        return plain("Нет связи с Gemini")
    if r.status_code == 429:
        return plain("Лимит запросов исчерпан")
    if r.status_code != 200:
        print(f"Gemini {r.status_code}: {r.text[:400]}", flush=True)
        return plain(f"Ошибка API {r.status_code}")

    answer = tidy(extract_text(r.json()))
    print(f"<- {answer}", flush=True)
    return plain(answer)


if __name__ == "__main__":
    if not API_KEY:
        sys.exit("Не задан GEMINI_API_KEY")
    if "--list" in sys.argv:
        list_models()
    else:
        app.run(host="0.0.0.0", port=8000)
