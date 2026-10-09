"""
Агент на ПК — читает команды из ntfy.sh и выполняет системные действия.
Включить → обрабатывает ESP32, этот скрипт игнорирует.
Выключить / Сон / Перезагрузка / Заблокировать / Статус / Скрин → этот скрипт.

Дополнительно: держит TCP-сокет на порту 8765 — ESP32 пингует его
чтобы понять включён ли компьютер (без внешних библиотек).
"""

import json
import os
import socket
import subprocess
import threading
import time
from collections import deque

import requests

# Топик берётся из NTFY_TOPIC или из файла .ntfy_topic рядом со скриптом (он в .gitignore)
NTFY_TOPIC        = os.environ.get("NTFY_TOPIC") or open(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".ntfy_topic")).read().strip()
STATUS_PORT       = 8765

# Такой же список синонимов — в MP-esp.ino (ALIASES), менять вместе.
# Сравнение по началу сообщения: "вкл", "Выключи!", "/reboot" и т.п.
COMMAND_ALIASES = [
    ("выкл", "Выключить"), ("выруб", "Выключить"), ("off", "Выключить"), ("shutdown", "Выключить"),
    ("вкл", "Включить"), ("вруб", "Включить"), ("on", "Включить"), ("wake", "Включить"), ("разбуд", "Включить"),
    ("сон", "Сон"), ("спат", "Сон"), ("усып", "Сон"), ("sleep", "Сон"), ("suspend", "Сон"),
    ("перезагр", "Перезагрузка"), ("перезапус", "Перезагрузка"), ("ребут", "Перезагрузка"),
    ("reboot", "Перезагрузка"), ("restart", "Перезагрузка"),
    ("заблок", "Заблокировать"), ("блок", "Заблокировать"), ("lock", "Заблокировать"),
    ("статус", "Статус"), ("status", "Статус"), ("инфо", "Статус"), ("info", "Статус"),
    ("скрин", "Скрин"), ("снимок", "Скрин"), ("screen", "Скрин"),
]


def canonical_command(text: str) -> str:
    s = text.strip().lower().replace("ё", "е").lstrip("/")
    for stem, cmd in COMMAND_ALIASES:
        if s.startswith(stem):
            return cmd
    return ""


COMMANDS = {
    "Выключить":     ["systemctl", "poweroff"],
    "Сон":           ["systemctl", "suspend"],
    "Перезагрузка":  ["systemctl", "reboot"],
    "Заблокировать": ["loginctl", "lock-session"],
}

last_time = int(time.time())
seen_ids: deque = deque(maxlen=64)
_screenshot_lock = threading.Lock()
_last_screenshot_time = 0.0


# ── ntfy publish ──────────────────────────────────────────────────────────────

def ntfy_publish(msg: str) -> None:
    try:
        # тег robot: ESP не принимает такие сообщения за команды
        requests.post(f"https://ntfy.sh/{NTFY_TOPIC}", data=msg.encode("utf-8"),
                      headers={"Tags": "robot"}, timeout=10)
    except Exception as e:
        print(f"[ntfy] publish error: {e}")


def upload_and_notify(path: str) -> None:
    with open(path, "rb") as f:
        r = requests.post(
            "https://uguu.se/upload",
            files={"files[]": f},
            timeout=30,
        )
    if r.status_code == 200:
        url = r.json()["files"][0]["url"]
        ntfy_publish(f"Скрин: {url}")
    else:
        ntfy_publish(f"Скрин: ошибка загрузки HTTP {r.status_code}")


# ── TCP статус-сервер (ESP32 подключается сюда чтобы проверить онлайн ли ПК) ──

def _status_server() -> None:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("", STATUS_PORT))
        s.listen(10)
        print(f"[status] TCP сервер на порту {STATUS_PORT}")
        while True:
            conn, _ = s.accept()
            conn.close()


def start_status_server() -> None:
    threading.Thread(target=_status_server, daemon=True).start()


# ── Статус ────────────────────────────────────────────────────────────────────

def get_status_text() -> str:
    try:
        uptime = subprocess.check_output(["uptime", "-p"], text=True).strip()
    except Exception:
        uptime = "?"

    try:
        mem_lines = subprocess.check_output(["free", "-h", "--si"], text=True).splitlines()
        mem = [l for l in mem_lines if l.startswith("Mem:")][0].split()
        ram = f"{mem[2]}/{mem[1]}"
    except Exception:
        ram = "?"

    try:
        load = open("/proc/loadavg").read().split()[:3]
        load_str = " ".join(load)
    except Exception:
        load_str = "?"

    try:
        who = subprocess.check_output(["who"], text=True).strip() or "нет активных сессий"
    except Exception:
        who = "?"

    return f"Аптайм: {uptime}\nRAM: {ram}\nНагрузка: {load_str}\nСессии:\n{who}"


# ── Скриншот ──────────────────────────────────────────────────────────────────

def take_screenshot() -> None:
    global _last_screenshot_time

    if not _screenshot_lock.acquire(blocking=False):
        print("[screenshot] already running, skipping")
        return

    try:
        now = time.time()
        if now - _last_screenshot_time < 30:
            print("[screenshot] cooldown, skipping")
            return
        _last_screenshot_time = now

        from PIL import Image

        raw = "/tmp/pc_agent_screenshot_raw.png"
        out = "/tmp/pc_agent_screenshot.jpg"

        for cmd in [["spectacle", "-b", "-n", "-o", raw], ["grim", raw]]:
            try:
                r = subprocess.run(cmd, capture_output=True, timeout=15)
                if r.returncode == 0 and os.path.exists(raw):
                    break
            except (FileNotFoundError, subprocess.TimeoutExpired):
                continue
        else:
            ntfy_publish("Скрин: spectacle/grim не найдены")
            return

        try:
            img = Image.open(raw)
            if img.width > 1920:
                ratio = 1920 / img.width
                img = img.resize((1920, int(img.height * ratio)), Image.LANCZOS)
            rgb = img.convert("RGB")
            img.close()
            del img
            rgb.save(out, "JPEG", quality=70, optimize=True)
            rgb.close()
            del rgb
        except Exception as e:
            ntfy_publish(f"Скрин: ошибка сжатия: {e}")
            return
        finally:
            try:
                os.unlink(raw)
            except OSError:
                pass

        try:
            upload_and_notify(out)
        except Exception as e:
            ntfy_publish(f"Скрин не отправлен: {e}")
        finally:
            try:
                os.unlink(out)
            except OSError:
                pass
    finally:
        _screenshot_lock.release()


# ── ntfy подписка ─────────────────────────────────────────────────────────────
# Одно постоянное соединение вместо опроса: опрос раз в 5 с вместе с ESP32
# упирался в лимит запросов ntfy.sh на один IP.

def handle_command(cmd: str) -> None:
    if cmd in COMMANDS:
        subprocess.run(COMMANDS[cmd])
    elif cmd == "Статус":
        ntfy_publish(get_status_text())
    elif cmd == "Скрин":
        threading.Thread(target=take_screenshot, daemon=True).start()


def subscribe() -> None:
    global last_time
    with requests.get(
        f"https://ntfy.sh/{NTFY_TOPIC}/json",
        params={"since": str(last_time)},
        stream=True,
        timeout=(10, 90),  # keepalive от ntfy каждые 45 с — тишина 90 с значит соединение мертво
    ) as r:
        r.raise_for_status()
        print("[ntfy] подписан")
        for line in r.iter_lines():
            if not line:
                continue
            try:
                data = json.loads(line)
            except json.JSONDecodeError:
                continue

            if data.get("event") != "message":
                continue

            # since включает текущую секунду, поэтому дубли отсекаем по id
            msg_id = data.get("id")
            if msg_id in seen_ids:
                continue
            seen_ids.append(msg_id)
            last_time = max(last_time, data["time"])

            if "robot" in data.get("tags", []):
                continue

            text = data.get("message", "").strip()
            cmd = canonical_command(text)
            print(f"[ntfy] {text!r} -> {cmd or '?'}")
            if cmd:
                handle_command(cmd)


if __name__ == "__main__":
    print(f"[PC Agent] Топик: {NTFY_TOPIC}")
    ntfy_publish("Компьютер загрузился")
    start_status_server()
    backoff = 5
    while True:
        try:
            subscribe()
            backoff = 5
        except Exception as e:
            print(f"[error] {e}")
            backoff = min(backoff * 2, 60)
        time.sleep(backoff)
