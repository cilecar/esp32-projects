import subprocess
from flask import Flask, request
import logging
import sys

# Настраиваем логирование: пишем и в файл, и выводим в консоль для удобства
logging.basicConfig(
    level=logging.INFO, 
    format='%(asctime)s - %(message)s',
    handlers=[
        logging.FileHandler("hub.log"),
        logging.StreamHandler(sys.stdout)
    ]
)

app = Flask(__name__)

@app.route('/sleep', methods=['GET'])
def sleep_system():
    logging.info("Получена команда на сон.")
    try:
        subprocess.run(["systemctl", "suspend"], check=True)
        return "Goodnight", 200
    except Exception as e:
        logging.error(f"Ошибка ухода в сон: {e}")
        return "Error", 500

@app.route('/log', methods=['POST'])
def receive_log():
    data = request.get_data(as_text=True)
    logging.info(f"ESP32: {data}")
    return "OK", 200

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000)