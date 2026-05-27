/*
 * client/network.c – сетевой модуль клиента.
 *
 * Отвечает за:
 *   - установку TCP-соединения с сервером (connect_to_server);
 *   - отправку строк протокола через сокет (send_cmd);
 *   - приём строк из сокета в отдельном потоке (network_thread);
 *   - буферизацию входящих сообщений в общей очереди (in_queue),
 *     защищённой мьютексом для безопасного доступа из главного потока.
 */

#include "client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ncurses.h>      /* для ungetch (в данной версии не используется) */

/* ---------- глобальная очередь входящих сообщений ---------- */
msg_queue_t in_queue = {NULL, 0, 0, PTHREAD_MUTEX_INITIALIZER};
int sockfd = -1;                     /* дескриптор сокета */
volatile int connected = 0;          /* флаг: 1 – соединение активно */

/*
 * queue_push – добавить строку msg в конец очереди in_queue.
 *
 * Потокобезопасна: захватывает in_queue.mutex.
 * При необходимости автоматически расширяет динамический массив.
 */
static void queue_push(const char *msg) {
    pthread_mutex_lock(&in_queue.mutex);

    /* расширяем массив, если заполнен */
    if (in_queue.count >= in_queue.capacity) {
        in_queue.capacity = in_queue.capacity ? in_queue.capacity * 2 : 16;
        in_queue.messages = realloc(in_queue.messages,
                                    in_queue.capacity * sizeof(msg_node_t));
    }

    /* копируем строку (с ограничением длины) */
    strncpy(in_queue.messages[in_queue.count].text, msg, MAX_MSG_LINE - 1);
    in_queue.messages[in_queue.count].text[MAX_MSG_LINE - 1] = '\0';
    in_queue.count++;

    pthread_mutex_unlock(&in_queue.mutex);
}

/*
 * read_line_socket – прочитать одну строку из сокета (до '\n').
 *
 * Читает побайтово, игнорирует '\r' (для совместимости с Windows).
 * Возвращает длину строки (без '\n'), или -1 при ошибке/отключении.
 */
static int read_line_socket(int fd, char *buf, size_t size) {
    size_t i = 0;
    while (i < size - 1) {
        char c;
        int n = recv(fd, &c, 1, 0);
        if (n <= 0) return -1;        /* ошибка или отключение */
        if (c == '\n') {
            buf[i] = '\0';
            return i;
        }
        if (c != '\r') {
            buf[i++] = c;
        }
    }
    buf[i] = '\0';
    return i;
}

/*
 * network_thread – поток, непрерывно читающий строки из сокета.
 *
 * Каждая прочитанная строка (одно сообщение протокола) помещается
 * в очередь in_queue. При обрыве соединения устанавливает
 * connected = 0, что заставляет главный поток завершить работу.
 */
void *network_thread(void *arg) {
    (void)arg;                        /* параметр не используется */
    char line[MAX_MSG_LINE];

    while (connected) {
        int len = read_line_socket(sockfd, line, sizeof(line));
        if (len < 0) {                /* сервер отключился или ошибка */
            connected = 0;
            break;
        }
        if (len > 0) {
            queue_push(line);         /* добавить в очередь для главного потока */
        }
    }
    return NULL;
}

/*
 * connect_to_server – создать TCP-сокет и подключиться к серверу.
 *
 * Параметры:
 *   ip   – IP-адрес сервера (строка, например "127.0.0.1")
 *   port – номер порта
 *
 * Возвращает 0 при успехе, -1 при ошибке.
 * Устанавливает глобальную переменную sockfd и флаг connected = 1.
 */
int connect_to_server(const char *ip, int port) {
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return -1; }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);          /* перевод в сетевой порядок байт */
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        perror("inet_pton");
        close(sockfd);
        return -1;
    }

    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(sockfd);
        return -1;
    }

    connected = 1;                          /* соединение установлено */
    return 0;
}

/*
 * send_cmd – отправить строку протокола через сокет.
 *
 * Параметры:
 *   buf – строка, которую нужно отправить (обычно уже с '\n' на конце)
 *
 * При ошибке отправки сбрасывает флаг connected в 0.
 */
void send_cmd(const char *buf) {
    if (connected && sockfd >= 0) {
        int len = strlen(buf);
        int sent = send(sockfd, buf, len, 0);
        if (sent < 0) {
            perror("send");
            connected = 0;             /* соединение разорвано */
        }
    }
}