import os
import threading
import telebot
from telebot.types import ReplyKeyboardMarkup, KeyboardButton
from flask import Flask, request

TOKEN = os.environ.get("BOT_TOKEN")
CHAT_ID = os.environ.get("CHAT_ID")

bot = telebot.TeleBot(TOKEN)
app = Flask(__name__)

system_armed = False

def get_keyboard():
    markup = ReplyKeyboardMarkup(resize_keyboard=True)
    markup.add(KeyboardButton("🟢 Включить защиту"), KeyboardButton("🔴 Выключить защиту"))
    return markup

@bot.message_handler(commands=['start', 'status'])
def send_status(message):
    status = "ВКЛЮЧЕНА 🟢" if system_armed else "ВЫКЛЮЧЕНА 🔴"
    bot.send_message(message.chat.id, f"Статус: {status}", reply_markup=get_keyboard())

@bot.message_handler(func=lambda m: m.text in ["🟢 Включить защиту", "🔴 Выключить защиту"])
def handle_toggle(message):
    global system_armed
    system_armed = (message.text == "🟢 Включить защиту")
    status = "ВКЛЮЧЕНА 🟢" if system_armed else "ВЫКЛЮЧЕНА 🔴"
    bot.send_message(message.chat.id, f"Принято. Статус: {status}", reply_markup=get_keyboard())

@app.route('/esp-alert', methods=['GET'])
def alert():
    event = request.args.get('event', 'motion')
    distance = request.args.get('distance', '')
    text = f"🚨 Тревога! Объект: {distance} см" if event == 'ultrasonic' else "🚨 Тревога! Движение"
    bot.send_message(CHAT_ID, text)
    return "OK", 200

@app.route('/esp-sync', methods=['GET'])
def sync():
    global system_armed
    set_state = request.args.get('set_state')
    if set_state is not None:
        system_armed = (set_state == '1')
        status = "ВКЛЮЧЕНА 🟢" if system_armed else "ВЫКЛЮЧЕНА 🔴"
        bot.send_message(CHAT_ID, f"🔄 Изменено на плате: {status}", reply_markup=get_keyboard())
        return "OK", 200
    return "1" if system_armed else "0", 200

def run_flask():
    app.run(host='0.0.0.0', port=int(os.environ.get('PORT', 80)))

if __name__ == '__main__':
    threading.Thread(target=run_flask, daemon=True).start()
    bot.polling(none_stop=True)