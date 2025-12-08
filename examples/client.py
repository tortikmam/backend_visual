import socket

def main():
    HOST = '127.0.0.1'
    PORT = 8888

    client_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    client_socket.connect((HOST, PORT))

    message = input()

    client_socket.sendall(message.encode('utf-8'))    
    response = client_socket.recv(1024)

    client_socket.close()

if __name__ == "__main__":
    main()