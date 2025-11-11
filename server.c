
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>

#define PORT 8080
#define BUF_SIZE 1024
#define MAX_CLIENTS 100
#define NAME_LEN 32

// Estados de cliente
#define STATE_NAME      0  // esperando nombre de usuario
#define STATE_READY     1  // listo (apertura de sesiones por comandos)

// =======================
// Estructuras y helpers
// =======================
typedef struct {
    int fd;                          // descriptor de socket
    char name[NAME_LEN];             // nombre del usuario
    int state;                       // estado
    // Soporte de sesiones múltiples: bitset simple (0/1) para cada otro cliente
    unsigned char sessions[MAX_CLIENTS + 1];
} client_t;

// Envía al cliente i la lista de usuarios disponibles
static void send_user_list(int i, struct pollfd fds[], client_t clients[]) {
    char buf[BUF_SIZE];
    int len = snprintf(buf, sizeof(buf), "Usuarios conectados:\n");
    for (int j = 1; j <= MAX_CLIENTS; j++) {
        if (j == i) continue;
        if (clients[j].fd >= 0 && clients[j].state >= STATE_READY) {
            len += snprintf(buf + len, sizeof(buf) - len, " - %s\n", clients[j].name);
            if (len >= (int)sizeof(buf)) break;
        }
    }
    len += snprintf(buf + len, sizeof(buf) - len,
                    "Comandos: /who, /sessions, /open <user>, /close <user>, /to <user> <msg>, /broadcast <msg>, /exit\n");
    send(clients[i].fd, buf, len, 0);
}

static int is_name_taken(const char *name, client_t clients[]) {
    for (int k = 1; k <= MAX_CLIENTS; k++) {
        if (clients[k].fd >= 0 && clients[k].state >= STATE_READY) {
            if (strncmp(clients[k].name, name, NAME_LEN) == 0) return 1;
        }
    }
    return 0;
}

static int find_by_name(const char *name, client_t clients[]) {
    for (int k = 1; k <= MAX_CLIENTS; k++) {
        if (clients[k].fd >= 0 && clients[k].state >= STATE_READY) {
            if (strncmp(clients[k].name, name, NAME_LEN) == 0) return k;
        }
    }
    return -1;
}

static void open_session(int i, int j, client_t clients[]) {
    clients[i].sessions[j] = 1;
}
static void close_session(int i, int j, client_t clients[]) {
    clients[i].sessions[j] = 0;
}
static int has_any_session(int i, client_t clients[]) {
    for (int k = 1; k <= MAX_CLIENTS; k++) if (clients[i].sessions[k]) return 1;
    return 0;
}
static void send_sessions_list(int i, client_t clients[]) {
    char buf[BUF_SIZE];
    int len = snprintf(buf, sizeof(buf), "Sesiones abiertas con:\n");
    int none = 1;
    for (int k = 1; k <= MAX_CLIENTS; k++) {
        if (clients[i].sessions[k]) {
            none = 0;
            len += snprintf(buf + len, sizeof(buf) - len, " - %s\n", clients[k].name);
            if (len >= (int)sizeof(buf)) break;
        }
    }
    if (none) len += snprintf(buf + len, sizeof(buf) - len, "(ninguna)\n");
    send(clients[i].fd, buf, len, 0);
}

int main() {
    int listen_fd, nfds;
    struct sockaddr_in addr;
    struct pollfd fds[MAX_CLIENTS + 1];
    client_t clients[MAX_CLIENTS + 1];
    char buffer[BUF_SIZE];

    // 1) Crear y configurar socket escucha
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); exit(EXIT_FAILURE); }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(PORT);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(listen_fd); exit(EXIT_FAILURE);
    }
    if (listen(listen_fd, 16) < 0) {
        perror("listen"); close(listen_fd); exit(EXIT_FAILURE);
    }

    printf("Servidor (poll) escuchando puerto %d\n", PORT);

    // 2) Inicializar fds y clients
    fds[0].fd = listen_fd;
    fds[0].events = POLLIN;
    for (int i = 1; i <= MAX_CLIENTS; i++) {
        fds[i].fd = -1;
        clients[i].fd = -1;
        clients[i].state = -1;
        memset(clients[i].sessions, 0, sizeof(clients[i].sessions));
        clients[i].name[0] = '\0';
    }

    // 3) Bucle principal
    while (1) {
        nfds = poll(fds, MAX_CLIENTS + 1, -1);
        if (nfds < 0) { perror("poll"); break; }

        // 3a) Nueva conexión
        if (fds[0].revents & POLLIN) {
            int conn_fd = accept(listen_fd, NULL, NULL);
            if (conn_fd < 0) {
                perror("accept");
            } else {
                int i;
                for (i = 1; i <= MAX_CLIENTS; i++) {
                    if (fds[i].fd < 0) {
                        fds[i].fd = conn_fd;
                        fds[i].events = POLLIN;
                        clients[i].fd = conn_fd;
                        clients[i].state = STATE_NAME;
                        memset(clients[i].sessions, 0, sizeof(clients[i].sessions));
                        clients[i].name[0] = '\0';
                        const char *msg = "Bienvenido. Ingresa tu nombre:\n";
                        send(conn_fd, msg, strlen(msg), 0);
                        printf("Nuevo cliente fd=%d slot=%d\n", conn_fd, i);
                        break;
                    }
                }
                if (i > MAX_CLIENTS) {
                    const char *msg = "Servidor lleno. Intenta mas tarde.\n";
                    send(conn_fd, msg, strlen(msg), 0);
                    close(conn_fd);
                }
            }
            if (--nfds == 0) continue;
        }

        // 3b) Actividad en clientes
        for (int i = 1; i <= MAX_CLIENTS; i++) {
            if (fds[i].fd < 0) continue;
            if (!(fds[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL))) continue;

            int fd = fds[i].fd;

            // Manejar desconexión por eventos de error/HUP
            if (fds[i].revents & (POLLHUP | POLLERR | POLLNVAL)) {
                // notificar a sesiones vecinas
                for (int j = 1; j <= MAX_CLIENTS; j++) {
                    if (clients[j].fd >= 0 && clients[j].sessions[i]) {
                        const char *msg = "El otro usuario se desconecto.\n";
                        send(clients[j].fd, msg, strlen(msg), 0);
                        clients[j].sessions[i] = 0;
                    }
                }
                memset(clients[i].sessions, 0, sizeof(clients[i].sessions));
                printf("Cliente %s (fd=%d) desconectado (HUP/ERR)\n",
                       clients[i].name[0] ? clients[i].name : "(sin-nombre)", fd);
                close(fd);
                fds[i].fd = -1;
                clients[i].fd = -1;
                clients[i].state = -1;
                clients[i].name[0] = '\0';
                if (--nfds == 0) break;
                continue;
            }

            // Lectura normal
            ssize_t n = recv(fd, buffer, BUF_SIZE - 1, 0);
            if (n <= 0) {
                // desconexión limpia
                printf("Cliente %s (fd=%d) desconectado\n",
                       clients[i].name[0] ? clients[i].name : "(sin-nombre)", fd);
                for (int j = 1; j <= MAX_CLIENTS; j++) {
                    if (clients[j].fd >= 0 && clients[j].sessions[i]) {
                        const char *msg = "El otro usuario se desconecto.\n";
                        send(clients[j].fd, msg, strlen(msg), 0);
                        clients[j].sessions[i] = 0;
                    }
                }
                memset(clients[i].sessions, 0, sizeof(clients[i].sessions));
                close(fd);
                fds[i].fd = -1;
                clients[i].fd = -1;
                clients[i].state = -1;
                clients[i].name[0] = '\0';
                if (--nfds == 0) break;
                continue;
            }

            buffer[n] = '\0';
            while (n > 0 && (buffer[n-1] == '\n' || buffer[n-1] == '\r')) buffer[--n] = '\0';
            if (n == 0) { if (--nfds == 0) break; continue; }

            switch (clients[i].state) {
                case STATE_NAME: {
                    // Validación nombres únicos
                    if (buffer[0] == '\0') {
                        const char *msg = "Nombre vacio. Intenta de nuevo:\n";
                        send(fd, msg, strlen(msg), 0);
                        break;
                    }
                    if ((int)strlen(buffer) >= NAME_LEN) {
                        const char *msg = "Nombre muy largo. Max 31 chars. Intenta de nuevo:\n";
                        send(fd, msg, strlen(msg), 0);
                        break;
                    }
                    if (is_name_taken(buffer, clients)) {
                        const char *msg = "Nombre en uso. Ingresa otro nombre:\n";
                        send(fd, msg, strlen(msg), 0);
                        break;
                    }
                    strncpy(clients[i].name, buffer, NAME_LEN - 1);
                    clients[i].name[NAME_LEN - 1] = '\0';
                    clients[i].state = STATE_READY;
                    printf("Cliente fd=%d registrado como '%s'\n", fd, clients[i].name);
                    send_user_list(i, fds, clients);

                    // refrescar lista a todos los READY
                    for (int k = 1; k <= MAX_CLIENTS; k++) {
                        if (k == i) continue;
                        if (clients[k].fd >= 0 && clients[k].state == STATE_READY) {
                            send_user_list(k, fds, clients);
                        }
                    }
                    break;
                }

                default: { // STATE_READY: parser de comandos y compatibilidad
                    // Comandos
                    if (strncmp(buffer, "/who", 4) == 0) {
                        send_user_list(i, fds, clients);
                        break;
                    }
                    if (strncmp(buffer, "/sessions", 9) == 0) {
                        send_sessions_list(i, clients);
                        break;
                    }
                    if (strncmp(buffer, "/open ", 6) == 0) {
                        const char *target = buffer + 6;
                        int j = find_by_name(target, clients);
                        if (j < 0 || j == i) {
                            const char *msg = "Usuario no disponible. Usa /who para listar.\n";
                            send(clients[i].fd, msg, strlen(msg), 0);
                            break;
                        }
                        open_session(i, j, clients);
                        open_session(j, i, clients); // conveniencia
                        const char *ok = "Sesion abierta.\n";
                        send(clients[i].fd, ok, strlen(ok), 0);
                        break;
                    }
                    if (strncmp(buffer, "/close ", 7) == 0) {
                        const char *target = buffer + 7;
                        int j = find_by_name(target, clients);
                        if (j < 0) {
                            const char *msg = "No existe esa sesion.\n";
                            send(clients[i].fd, msg, strlen(msg), 0);
                            break;
                        }
                        close_session(i, j, clients);
                        close_session(j, i, clients);
                        const char *ok = "Sesion cerrada.\n";
                        send(clients[i].fd, ok, strlen(ok), 0);
                        break;
                    }
                    if (strncmp(buffer, "/to ", 4) == 0) {
                        const char *p = buffer + 4;
                        const char *sp = strchr(p, ' ');
                        if (!sp) {
                            const char *msg = "Uso: /to <usuario> <mensaje>\n";
                            send(clients[i].fd, msg, strlen(msg), 0);
                            break;
                        }
                        char uname[NAME_LEN];
                        size_t ulen = (size_t)(sp - p);
                        if (ulen >= NAME_LEN) ulen = NAME_LEN - 1;
                        strncpy(uname, p, ulen); uname[ulen] = '\0';

                        int j = find_by_name(uname, clients);
                        if (j < 0 || !clients[i].sessions[j]) {
                            const char *msg = "No hay sesion con ese usuario. Usa /open <usuario>.\n";
                            send(clients[i].fd, msg, strlen(msg), 0);
                            break;
                        }
                        const char *payload = sp + 1;
                        if (clients[j].fd >= 0) {
                            char forward[BUF_SIZE];
                            int L = snprintf(forward, sizeof(forward), "[%s] %s\n", clients[i].name, payload);
                            send(clients[j].fd, forward, L, 0);
                        }
                        break;
                    }
                    if (strncmp(buffer, "/broadcast ", 11) == 0) {
                        const char *payload = buffer + 11;
                        for (int j = 1; j <= MAX_CLIENTS; j++) {
                            if (clients[i].sessions[j] && clients[j].fd >= 0) {
                                char forward[BUF_SIZE];
                                int L = snprintf(forward, sizeof(forward), "[%s] %s\n", clients[i].name, payload);
                                send(clients[j].fd, forward, L, 0);
                            }
                        }
                        break;
                    }
                    if (strcmp(buffer, "/exit") == 0) {
                        for (int j = 1; j <= MAX_CLIENTS; j++) {
                            if (clients[i].sessions[j]) {
                                close_session(i, j, clients);
                                close_session(j, i, clients);
                            }
                        }
                        const char *msg = "Has cerrado todas tus sesiones.\n";
                        send(clients[i].fd, msg, strlen(msg), 0);
                        break;
                    }

                    // Compatibilidad con flujo antiguo:
                    // - Si escribe SOLO un nombre ⇒ /open <nombre>
                    if (!strchr(buffer, ' ')) {
                        int j = find_by_name(buffer, clients);
                        if (j > 0 && j != i) {
                            open_session(i, j, clients);
                            open_session(j, i, clients);
                            const char *ok = "Sesion abierta.\n";
                            send(clients[i].fd, ok, strlen(ok), 0);
                            break;
                        }
                    }
                    // - Si hay exactamente 1 sesión abierta, envía allí
                    int last = -1, count = 0;
                    for (int j = 1; j <= MAX_CLIENTS; j++) if (clients[i].sessions[j]) { last = j; count++; }
                    if (count == 1 && last > 0 && clients[last].fd >= 0) {
                        char forward[BUF_SIZE];
                        int L = snprintf(forward, sizeof(forward), "[%s] %s\n", clients[i].name, buffer);
                        send(clients[last].fd, forward, L, 0);
                    } else if (count > 1) {
                        const char *msg = "Varias sesiones abiertas. Usa /to <usuario> <mensaje> o /broadcast <mensaje>.\n";
                        send(clients[i].fd, msg, strlen(msg), 0);
                    } else {
                        const char *msg = "No tienes sesiones. Usa /open <usuario> o /who para listar.\n";
                        send(clients[i].fd, msg, strlen(msg), 0);
                    }
                    break;
                }
            }
            if (--nfds == 0) break;
        }
    }

    close(listen_fd);
    return 0;
}
