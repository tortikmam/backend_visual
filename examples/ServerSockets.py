import zmq
import time
import os

LOG_FILE = "android_data_log.txt"

def run_server():

    context = zmq.Context()

    socket = context.socket(zmq.REP) 

    try:
        socket.bind("tcp://0.0.0.0:8080") 
        print("Сервер запущен")
    except zmq.ZMQError as e:
        print(f"Ошибка")
        return
        
    counter = 0

    try:
        while True:
            message = socket.recv()
            message_str = message.decode('utf-8')
            counter += 1
            
            print(f"\n[SERVER] Получен пакет #{counter}: '{message_str}'")

            timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
            log_entry = f"{timestamp} | Packet #{counter}: {message_str}\n"
            
            with open(LOG_FILE, "a") as f: 
                f.write(log_entry)
            
            print(f"[SERVER] Данные сохранены в {LOG_FILE}.")

            reply = "Hello from Server!"
            socket.send_string(reply)
            print(f"[SERVER] Отправлен ответ: '{reply}'")

    except KeyboardInterrupt:
        print("\nСервер остановлен пользователем.")
    finally:
        socket.close()
        context.term()
        print("\nРесурсы очищены.")

def display_saved_data():
    if os.path.exists(LOG_FILE):
        print("\n=======================================================")
        print("       СОХРАНЕННЫЕ ДАННЫЕ В ФАЙЛЕ LOG.TXT       ")
        print("=======================================================")
        with open(LOG_FILE, "r") as f:
            print(f.read())
        print("=======================================================")

if __name__ == "__main__": 
    runOrDisplay = int(input("1 - запустить сервер, 2 - посмотреть файл с данными: "))
        
    if runOrDisplay == 1:   
        run_server()
    elif runOrDisplay == 2:
        display_saved_data()