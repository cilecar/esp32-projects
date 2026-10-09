from flask import Flask, jsonify, request
import psutil
import glob
import requests
import subprocess
import shutil
import socket
import time
import urllib.request
from threading import Thread
from queue import Queue
import logging

ESP32_IP = '192.168.31.13'
UDP_PORT = 4210

app = Flask(__name__)
ddc_queue = Queue()

state = {
    "cpu": 0, "gpu": 0, "ram_used": 0, "ram_total": 0,
    "disk_free": 0, "weather_temp": "--", "weather_desc": "Loading...", "media": "Loading..."
}

# --- ВОРКЕР DDCUTIL ---
def ddc_worker():
    print("[DDC] Воркер запущен и ждет команд...")
    while True:
        cmd = ddc_queue.get()
        print(f"[DDC] Выполняю команду: {' '.join(cmd)}")
        start_t = time.time()
        
        # Запускаем команду и ждем вывода
        result = subprocess.run(cmd, capture_output=True, text=True)
        
        exec_t = round(time.time() - start_t, 2)
        if result.returncode != 0:
            print(f"[DDC ОШИБКА] ({exec_t}s) Код {result.returncode}: {result.stderr.strip()}")
        else:
            print(f"[DDC УСПЕХ] ({exec_t}s) Яркость изменена!")
            
        ddc_queue.task_done()

# --- ОСТАЛЬНЫЕ ФУНКЦИИ ---
def get_gpu_usage():
    for path in glob.glob('/sys/class/drm/card*/device/gpu_busy_percent'):
        try:
            with open(path, 'r') as f:
                data = f.read().strip()
                if data.isdigit(): return int(data)
        except: continue
    return 0

def get_weather():
    try:
        headers = {'User-Agent': 'curl/8.0.0'}
        r = requests.get('http://wttr.in/Krasnodar?format=%t|%C', headers=headers, timeout=5)
        if r.status_code != 200: return "N/A", "Limit"
        text = r.text.strip().replace('°C', 'C').replace('°', '')
        parts = text.split('|')
        if len(parts) == 2: return parts[0].strip(), parts[1].strip()
        return "N/A", "Parse Err"
    except: return "N/A", "Req Err"

def get_media():
    try:
        res = subprocess.run(['playerctl', '--player=spotify,%any', 'metadata', '--format', '{{artist}} - {{title}}'], capture_output=True, text=True, timeout=1)
        return res.stdout.strip() if res.returncode == 0 else "None"
    except FileNotFoundError: return "playerctl Req"
    except: return "Err"

# --- ВОРКЕРЫ СТАТИСТИКИ ---
def worker_fast_stats():
    while True:
        ram = psutil.virtual_memory()
        disk = shutil.disk_usage("/")
        state["cpu"] = int(psutil.cpu_percent(interval=1))
        state["gpu"] = get_gpu_usage()
        state["ram_used"] = int(ram.used / (1024**3))
        state["ram_total"] = int(ram.total / (1024**3))
        state["disk_free"] = int(disk.free / (1024**3))
        state["media"] = get_media()

def worker_weather():
    while True:
        temp, desc = get_weather()
        state["weather_temp"] = temp
        state["weather_desc"] = desc
        time.sleep(600)

def worker_ping():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    while True:
        start = time.time()
        try:
            urllib.request.urlopen('http://gstatic.com/generate_204', timeout=2)
            ping_ms = int((time.time() - start) * 1000)
        except: ping_ms = 999 
        try: sock.sendto(f"{ping_ms}\n".encode('utf-8'), (ESP32_IP, UDP_PORT))
        except: pass
        time.sleep(1)

# --- РОУТЫ ---
@app.route('/api/pc')
def get_pc_stats():
    return jsonify(state)

@app.route('/api/brightness', methods=['POST'])
def set_brightness():
    print(f"\n[API] Входящий запрос на яркость: {request.data}")
    data = request.get_json(force=True, silent=True)
    
    if not data:
        print("[API ОШИБКА] Не удалось распарсить JSON!")
        return jsonify({"status": "error", "msg": "invalid json"}), 400
        
    print(f"[API] Разобранный JSON: {data}")
    if 'd1' in data:
        ddc_queue.put(['ddcutil', '--display', '1', 'setvcp', '10', str(data['d1'])])
    if 'd2' in data:
        ddc_queue.put(['ddcutil', '--display', '2', 'setvcp', '10', str(data['d2'])])
        
    return jsonify({"status": "queued"})

if __name__ == '__main__':
    Thread(target=ddc_worker, daemon=True).start()
    Thread(target=worker_fast_stats, daemon=True).start()
    Thread(target=worker_weather, daemon=True).start()
    Thread(target=worker_ping, daemon=True).start()
    
    # Логи Flask включены для дебага
    app.run(host='0.0.0.0', port=5000)