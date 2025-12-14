#include <stdio.h>
#include <string.h>
#include <winsock2.h>

#pragma comment(lib, "ws2_32.lib")

int main() {
    WSADATA wsa;
    SOCKET sock;
    struct sockaddr_in server;
    char message[] = "Hello World!";
    char buffer[1024];

    
    WSAStartup(MAKEWORD(2,2), &wsa);

    
    sock = socket(AF_INET, SOCK_STREAM, 0);

    server.sin_addr.s_addr = inet_addr("127.0.0.1");
    server.sin_family = AF_INET;
    server.sin_port = htons(5000);

    
    connect(sock, (struct sockaddr *)&server, sizeof(server));

    
    send(sock, message, strlen(message), 0);

    
    int bytes = recv(sock, buffer, sizeof(buffer)-1, 0);
    buffer[bytes] = '\0';

    printf("Получено: %s\n", buffer);

    closesocket(sock);
    WSACleanup();
    return 0;
}
