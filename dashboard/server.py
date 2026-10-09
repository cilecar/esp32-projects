from flask import Flask, jsonify
import psutil
import glob
import requests
import subprocess
import shutil

app = Flask(__name__)

def get_gpu_usage():
    for path in glob.glob('/sys/class/drm/card*/device/gpu_busy_percent'):
        try:
            with open(path, 'r') as f:
                data = f.read().strip()
                if data.isdigit():
                    return int(data)
        except (FileNotFoundError, ValueError, PermissionError):
            continue
    return 0

def get_weather():
    try:
        r = requests.get('http://wttr.in/Krasnodar?format=%t|%C', timeout=5)
        text = r.text.strip().replace('°C', 'C').replace('°', '')
        text = text.encode('ascii', 'ignore').decode('ascii')
        parts = text.split('|')
        if len(parts) == 2:
            return parts[0].strip(), parts[1].strip()
        return "N/A", "Error"
    except:
        return "N/A", "Error"

def get_media():
    try:
        # Указываем приоритет: сначала ищем Spotify, затем любой другой плеер
        res = subprocess.run(
            ['playerctl', '--player=spotify,%any', 'metadata', '--format', '{{artist}} - {{title}}'],
            capture_output=True, text=True, timeout=1
        )
        return res.stdout.strip() if res.returncode == 0 else "None"
    except FileNotFoundError:
        return "playerctl Req"
    except:
        return "Err"

@app.route('/api/pc')
def get_pc_stats():
    ram = psutil.virtual_memory()
    temp, desc = get_weather()
    disk = shutil.disk_usage("/")
    
    return jsonify({
        "cpu": int(psutil.cpu_percent(interval=0.1)),
        "gpu": get_gpu_usage(),
        "ram_used": int(ram.used / (1024**3)),
        "ram_total": int(ram.total / (1024**3)),
        "disk_free": int(disk.free / (1024**3)),
        "weather_temp": temp,
        "weather_desc": desc,
        "media": get_media()
    })

if __name__ == '__main__':
    import logging
    log = logging.getLogger('werkzeug')
    log.setLevel(logging.ERROR)
    app.run(host='0.0.0.0', port=5000)