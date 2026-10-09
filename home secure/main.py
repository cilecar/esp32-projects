import network
import urequests
import ujson
import time
import gc 
from machine import Pin, PWM, I2C, time_pulse_us, ADC
import ssd1306
import bme280

from credentials import TOKEN, CHAT_ID, WIFI_SSID, WIFI_PASS  # credentials.py: см. credentials.py.example

bme = bme280.BME280(i2c=i2c)
pir = Pin(13, Pin.IN)
led = Pin(2, Pin.OUT)
buzzer = Pin(12, Pin.OUT)
trig = Pin(5, Pin.OUT)
echo = Pin(18, Pin.IN)
i2c = I2C(0, scl=Pin(22), sda=Pin(21))
oled = ssd1306.SSD1306_I2C(128, 64, i2c)
sensor_dht = dht.DHT22(Pin(15))
sensor_light = ADC(Pin(34))
sensor_light.atten(ADC.ATTN_11DB) 

is_armed = False 
last_pir_state = 0 
last_update_id = 0 
current_temp, current_hum, current_light = 0.0, 0.0, 0

btn_flag = False
last_irq_time = 0

def button_isr(pin): 
    global btn_flag, last_irq_time
    now = time.ticks_ms()
    if time.ticks_diff(now, last_irq_time) > 300: 
        btn_flag = True
        last_irq_time = now

button = Pin(4, Pin.IN, Pin.PULL_UP)
button.irq(trigger=Pin.IRQ_FALLING, handler=button_isr)

def update_screen(status, alarm=False, dist_text=""):
    oled.fill(0) 
    oled.text("Home security", 25, 0)
    oled.hline(0, 12, 128, 1) 
    oled.text("Sys: " + status, 0, 15)
    
    if is_armed:
        if dist_text: oled.text(dist_text, 0, 30)
        if alarm: oled.text("!!! MOTION !!!", 15, 45)
    else:
        oled.text(f"Temp: {current_temp} C", 0, 30)
        oled.text(f"Hum:  {current_hum} %", 0, 42)
        oled.text(f"Light: {current_light} %", 0, 54)
    oled.show()

update_screen("BOOTING...")
print("connecting wifi", end="")
wifi = network.WLAN(network.STA_IF)
wifi.active(True)
wifi.connect(WIFI_SSID, WIFI_PASS)
timeout = 20
while not wifi.isconnected() and timeout > 0:
    time.sleep(0.5)
    print(".", end="")
    timeout -= 1
if not wifi.isconnected():
    print("\nОшибка: не удалось подключиться к Wi-Fi")

def send_telegram_message(text):
    url = f"https://api.telegram.org/bot{TOKEN}/sendMessage"
    headers = {'Content-Type': 'application/json'}
    
    payload = ujson.dumps({"chat_id": CHAT_ID, "text": text}).encode('utf-8')
    
    try: 
        resp = urequests.post(url, headers=headers, data=payload)
        resp.close()
    except Exception as e: 
        print("Ошибка отправки:", e)
    gc.collect()

def flush_telegram():
    global last_update_id
    url = f"https://api.telegram.org/bot{TOKEN}/getUpdates"
    try:
        resp = urequests.get(url)
        data = resp.json()
        resp.close()
        if data.get('ok') and len(data.get('result', [])) > 0:
            last_update_id = data['result'][-1]['update_id']
    except: pass
    gc.collect()

def check_telegram():
    global last_update_id
    url = f"https://api.telegram.org/bot{TOKEN}/getUpdates?offset={last_update_id + 1}&timeout=0"
    try:
        resp = urequests.get(url)
        data = resp.json()
        resp.close()
        if data.get('ok'):
            for result in data.get('result', []):
                last_update_id = result['update_id']
                if result.get('message', {}).get('text', '') == '/status':
                    status_str = "ON" if is_armed else "OFF"
                    
                    reply = (f"Статус системы\n\n" +
                             f"Температура: {current_temp}°C\n" +
                             f"Влажность: {current_hum}%\n" +
                             f"Яркость: {current_light}%\n" +
                             f"Охрана: {status_str}")
                    
                    send_telegram_message(reply)
    except: pass
    gc.collect()

def read_climate():
    global current_temp, current_hum, current_light
    try:
        t, p, h = bme.values
        current_temp = t.replace('C', '')
        current_hum = h.replace('%', '')
        
        current_light = 100 - int((sensor_light.read() / 4095) * 100)
    except Exception as e: 
        print("BME Error:", e)

def get_distance():
    trig.value(0); time.sleep_us(5)
    trig.value(1); time.sleep_us(10); trig.value(0)
    try:
        pulse_time = time_pulse_us(echo, 1, 30000) 
        if pulse_time > 0: return int((pulse_time / 2) / 29.1)
        return 999
    except: return 999


flush_telegram() 
update_screen("OFF") 

last_tg_check = 0
last_climate_check = 0

while True:
    now = time.ticks_ms()
    gc.collect() 
    buzzer.value(0)

    if btn_flag:
        btn_flag = False 
        is_armed = not is_armed 
        
        if is_armed:
            update_screen("ON")
            buzzer.value(1); time.sleep(0.1); buzzer.value(0)
            send_telegram_message("Система активирована")
        else:
            update_screen("OFF")
            buzzer.value(1); time.sleep(0.1); buzzer.value(0); time.sleep(0.1)
            buzzer.value(1); time.sleep(0.1); buzzer.value(0)
            send_telegram_message("Система деактивирована")

    if time.ticks_diff(now, last_climate_check) > 2000:
        read_climate()
        if not is_armed: 
            update_screen("OFF")
        last_climate_check = now
        
    if time.ticks_diff(now, last_tg_check) > 3000:
        check_telegram()
        last_tg_check = now

    if is_armed:
        current_pir = pir.value()
        if current_pir == 1 and last_pir_state == 0:
            dist = get_distance()
            dist_str = f"Dist: {dist} cm"
            
            if dist < 100:
                for i in range(10):
                    if btn_flag:
                        is_armed = False
                        btn_flag = False
                        buzzer.value(0); led.value(0)
                        update_screen("OFF")
                        send_telegram_message("Система экстренно отключена")
                        break
                        
                    led.value(1); buzzer.value(1)
                    update_screen("ON", alarm=True, dist_text=dist_str) 
                    time.sleep(0.15)
                    led.value(0); buzzer.value(0)
                    update_screen("ON", alarm=False, dist_text=dist_str) 
                    time.sleep(0.15)
                    
                if is_armed: 
                    send_telegram_message(f"Обнаружено движение: {dist} см")
                    update_screen("ON") 
                    
        last_pir_state = current_pir
        
    time.sleep(0.05)