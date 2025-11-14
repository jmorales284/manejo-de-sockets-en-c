// client.c — Cliente Windows (Winsock2) con colores ANSI en consola
// Mensajes: recibido (verde), enviado (cian), sistema (amarillo)

#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <string.h>

#pragma comment(lib, "Ws2_32.lib")

#define SERVER_IP   "172.16.135.3"
#define SERVER_PORT 8080

#define C_RESET   "\x1b[0m"
#define C_SEND    "\x1b[36m"  // cian: mensajes que YO envío
#define C_RECV    "\x1b[32m"  // verde: mensajes recibidos
#define C_SYS     "\x1b[33m"  // amarillo: eventos del sistema

static void print_sent(const char *msg) { printf(C_SEND "[Yo] %s" C_RESET, msg); }
static void print_recv(const char *msg) { printf(C_RECV "%s" C_RESET, msg); }
static void print_sys (const char *msg) { printf(C_SYS  "%s" C_RESET, msg); }

static void trim_crlf(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) s[--n] = '\0';
}

DWORD WINAPI recibirMensajes(LPVOID lpParam) {
    SOCKET sock = *((SOCKET*)lpParam);
    char buffer[1024];
    int len;
    while ((len = recv(sock, buffer, sizeof(buffer) - 1, 0)) > 0) {
        buffer[len] = '\0';
        print_recv(buffer);
    }
    return 0;
}

int main(int argc, char **argv) {
    WSADATA wsaData;
    SOCKET sock;
    struct sockaddr_in server;
    char mensaje[1024];
    DWORD threadId;
    HANDLE hThread;

    const char *ip = SERVER_IP;
    int port = SERVER_PORT;

    if (argc >= 2) ip = argv[1];
    if (argc >= 3) port = atoi(argv[2]);

    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        print_sys("WSAStartup fallo.\n"); return 1;
    }

    // Habilitar ANSI en consola (Windows 10+)
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {
            mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            SetConsoleMode(hOut, mode);
        }
    }

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        print_sys("No se pudo crear socket.\n");
        WSACleanup(); return 1;
    }

    memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET;
    server.sin_port = htons((u_short)port);
    server.sin_addr.s_addr = inet_addr(ip);

    print_sys("Conectando...\n");
    if (connect(sock, (struct sockaddr*)&server, sizeof(server)) == SOCKET_ERROR) {
        print_sys("No se pudo conectar.\n");
        closesocket(sock);
        WSACleanup(); return 1;
    }
    print_sys("Conectado.\n");

    hThread = CreateThread(NULL, 0, recibirMensajes, &sock, 0, &threadId);

    // Prompt útil
    print_sys("Tips: /who, /open <user>, /to <user> <msg>, /broadcast <msg>, /close <user>, /exit\n");

    while (1) {
        if (!fgets(mensaje, sizeof(mensaje), stdin)) break;
        trim_crlf(mensaje);
        if (strncmp(mensaje, "/exit", 5) == 0) {
            send(sock, "/exit\n", 6, 0); // notifica al server
            break;
        }
        // Enviar
        if (send(sock, mensaje, (int)strlen(mensaje), 0) == SOCKET_ERROR) {
            print_sys("Error al enviar.\n");
            break;
        }
        print_sent(mensaje);
        printf("\n");
    }

    // Cierre ordenado
    shutdown(sock, SD_BOTH);
    if (hThread) {
        WaitForSingleObject(hThread, INFINITE);
        CloseHandle(hThread);
    }
    closesocket(sock);
    WSACleanup();
    print_sys("Desconectado.\n");
    return 0;
}
