
import socket

def main():
    
    HOST = '127.0.0.1'
    PORT = 8888         
    
    server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        
    server_socket.bind((HOST, PORT))
        
    server_socket.listen(1)
        
        
    while True:
        client_socket, client_address = server_socket.accept()

        data = client_socket.recv(1024)
        if data:
            message = data.decode('utf-8')
            print(f"Получено от клиента: '{message}'")
            
        response = "Hellow World!"
        client_socket.sendall(response.encode('utf-8'))
        print(f"Отправлено: '{response}'")
            
        client_socket.close()
        print(f"Соединение с {client_address} закрыто\n")
        print("Ожидание нового подключения...\n")
            

if __name__ == "__main__":
    main()