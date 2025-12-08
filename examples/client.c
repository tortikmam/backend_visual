#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>

#define HOST "127.0.0.1"
#define PORT 8888

int main() {
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);


    struct sockaddr_in server;

    server.sin_family = AF_INET;

    server.sin_port = htons(PORT);

    inet_pton(AF_INET, HOST, &server.sin_addr);

    connect(server_socket, (struct sockaddr*)&server, sizeof(server));

    char* message = "zdarov";

    send(server_socket, message, strlen(message), 0);

    char buffer[100];

    recv(server_socket, buffer, sizeof(buffer), 0);

    printf("Server: %s\n", buffer);
    close(server_socket);
    return 0;
}