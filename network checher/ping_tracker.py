import os
import re
import socket
import subprocess

ESP32_IP = '192.168.31.13'
UDP_PORT = 4210
# Сервер для проверки доступности интернета/VPN (можешь заменить на IP своего VPN-шлюза)
TARGET_HOST = '8.8.8.8' 

def get_icmp_ping(host):
    """Выполняет один ICMP-запрос и возвращает пинг в мс (float)."""
    try:
        # Принудительно задаем английскую локаль 'C' для предсказуемого вывода ping
        env = os.environ.copy()
        env["LC_ALL"] = "C"
        
        output = subprocess.check_output(
            ["ping", "-c", "1", "-W", "1", host],
            stderr=subprocess.STDOUT,
            text=True,
            env=env
        )
        match = re.search(r'time=([\d.]+)', output)
        if match:
            return float(match.group(1))
    except Exception:
        pass
    return None

def main():
    ping_val = get_icmp_ping(TARGET_HOST)
    
    # Формируем данные
    if ping_val is None:
        display_text = "ERR"
        send_val = 999
    else:
        # Для вывода в KDE оставляем красивые "ms"
        display_text = f"{ping_val:.1f} ms" if ping_val < 1.0 else f"{int(ping_val)} ms"
        # Для ESP32 округляем, чтобы не слать 0
        send_val = 1 if (0 < ping_val < 1) else int(ping_val)
        
    # Отправка на ESP32 
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.sendto(f"{send_val}\n".encode('utf-8'), (ESP32_IP, UDP_PORT))
    except Exception:
        pass # Игнорируем сетевые ошибки, чтобы не сломать вывод в панель KDE

    # Этот print() уходит напрямую в виджет Command Output
    print(display_text)

if __name__ == '__main__':
    main()