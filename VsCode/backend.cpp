#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "head.h"

int main() {
    const char* HOST = "127.0.0.1";
    const int PORT = 8888;
    
    // Создание сокета
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        std::cerr << "Ошибка создания сокета" << std::endl;
        return 1;
    }
    
    // Настройка адреса сервера
    sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(PORT);
    server_addr.sin_addr.s_addr = inet_addr(HOST);
    
    // Привязка сокета
    if (bind(server_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == -1) {
        std::cerr << "Ошибка привязки сокета" << std::endl;
        close(server_socket);
        return 1;
    }
    
    // Прослушивание порта
    if (listen(server_socket, 1) == -1) {
        std::cerr << "Ошибка прослушивания" << std::endl;
        close(server_socket);
        return 1;
    }
    
    std::cout << "Сервер запущен на " << HOST << ":" << PORT << std::endl;
    
    while (true) {
        sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        
        // Принятие соединения
        int client_socket = accept(server_socket, (struct sockaddr*)&client_addr, &client_addr_len);
        if (client_socket == -1) {
            std::cerr << "Ошибка принятия соединения" << std::endl;
            continue;
        }
        
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        
        // Получение данных от клиента
        char buffer[1024] = {0};
        int bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        
        if (bytes_received > 0) {
            std::string message(buffer);
            std::cout << "Получено от клиента: '" << message << "'" << std::endl;
        }
        
        // Отправка ответа
        std::string response = "Hellow World!";
        send(client_socket, response.c_str(), response.length(), 0);
        std::cout << "Отправлено: '" << response << "'" << std::endl;
        
        // Закрытие соединения
        close(client_socket);
        std::cout << "Соединение с " << client_ip << ":" << ntohs(client_addr.sin_port) << " закрыто" << std::endl;
        std::cout << "Ожидание нового подключения..." << std::endl << std::endl;
    }
    
    close(server_socket);
    return 0;
}