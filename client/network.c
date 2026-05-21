#include "client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ncurses.h>      // <-- для ungetch

msg_queue_t in_queue = {NULL, 0, 0, PTHREAD_MUTEX_INITIALIZER};
int sockfd = -1;
volatile int connected = 0;

static void queue_push(const char *msg) {
    pthread_mutex_lock(&in_queue.mutex);
    if (in_queue.count >= in_queue.capacity) {
        in_queue.capacity = in_queue.capacity ? in_queue.capacity*2 : 16;
        in_queue.messages = realloc(in_queue.messages,
                                    in_queue.capacity * sizeof(msg_node_t));
    }
    strncpy(in_queue.messages[in_queue.count].text, msg, MAX_MSG_LINE-1);
    in_queue.messages[in_queue.count].text[MAX_MSG_LINE-1] = '\0';
    in_queue.count++;
    pthread_mutex_unlock(&in_queue.mutex);
}

// Чтение строки из сокета (побайтово)
static int read_line_socket(int fd, char *buf, size_t size) {
    size_t i = 0;
    while (i < size - 1) {
        char c;
        int n = recv(fd, &c, 1, 0);
        if (n <= 0) return -1;
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

void *network_thread(void *arg) {
    (void)arg;
    char line[MAX_MSG_LINE];
    while (connected) {
        int len = read_line_socket(sockfd, line, sizeof(line));
        if (len < 0) {
            connected = 0;
            break;
        }
        if (len > 0) {
            queue_push(line);
        }
    }
    return NULL;
}

int connect_to_server(const char *ip, int port) {
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) { perror("socket"); return -1; }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) <= 0) {
        perror("inet_pton"); close(sockfd); return -1;
    }

    if (connect(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect"); close(sockfd); return -1;
    }

    connected = 1;
    return 0;
}

void send_cmd(const char *buf) {
    if (connected && sockfd >= 0) {
        int len = strlen(buf);
        int sent = send(sockfd, buf, len, 0);
        if (sent < 0) {
            perror("send");
            connected = 0;
        }
    }
}