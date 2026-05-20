#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "network.h"
#include "client.h"
#include "ui.h"

/* =========================================================
 * net_connect — подключиться к серверу
 * ========================================================= */
int net_connect(const char *host, int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);

    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        fprintf(stderr, "Неверный адрес: %s\n", host);
        close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }

    g_app.server_fd = fd;

    /* Два отдельных потока на один сокет:
     * write_stream — только для отправки (net_send_cmd)
     * read_stream  — только для чтения  (net_recv_thread)
     * Без dup() один поток мешает другому при одновременном доступе */
    int fd_dup = dup(fd);
    if (fd_dup < 0) {
        perror("dup");
        close(fd);
        return -1;
    }
    g_app.stream      = fdopen(fd,     "w");  /* запись */
    g_app.read_stream = fdopen(fd_dup, "r");  /* чтение */
    if (!g_app.stream || !g_app.read_stream) {
        perror("fdopen");
        close(fd);
        return -1;
    }
    g_app.connected = 1;

    /* Запускаем фоновый поток чтения входящих сообщений */
    if (pthread_create(&g_app.net_tid, NULL, net_recv_thread, NULL) != 0) {
        perror("pthread_create");
        fclose(g_app.stream);
        return -1;
    }
    pthread_detach(g_app.net_tid);

    return 0;
}

/* =========================================================
 * net_send_cmd — отправить команду серверу
 * ========================================================= */
void net_send_cmd(const char *cmd)
{
    if (!g_app.stream || !g_app.connected) return;

    pthread_mutex_lock(&g_app.msg_mutex);
    fprintf(g_app.stream, "%s\n", cmd);
    fflush(g_app.stream);  /* Критично: без этого строка остаётся в буфере */
    pthread_mutex_unlock(&g_app.msg_mutex);
}

/* =========================================================
 * net_recv_thread — фоновый поток: читает строки от сервера
 * ========================================================= */
void *net_recv_thread(void *arg)
{
    (void)arg;
    char buf[BUF_SIZE];

    while (g_app.connected && fgets(buf, sizeof(buf), g_app.read_stream)) {
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';

        /* Разбираем тип ответа */
        if (strncmp(buf, "INCOMING|", 9) == 0) {
            /* Входящее сообщение: INCOMING|chat_id=...|from=...|body=... */
            char from[MAX_LOGIN] = {0};
            char body[MAX_TEXT]  = {0};

            const char *p;
            if ((p = strstr(buf, "from=")) != NULL) {
                p += 5;
                size_t i = 0;
                while (*p && *p != '|' && i < sizeof(from)-1) from[i++] = *p++;
            }
            if ((p = strstr(buf, "body=")) != NULL) {
                p += 5;
                size_t i = 0;
                while (*p && *p != '|' && i < sizeof(body)-1) body[i++] = *p++;
            }
            ui_append_message(from, body);

        } else if (strncmp(buf, "CHATLIST_UPDATE|", 16) == 0) {
            /* Нас добавили в новый групповой чат */
            char chat_id_str[32] = {0};
            char name[MAX_CHAT_NAME] = {0};
            const char *p;
            if ((p = strstr(buf, "chat_id=")) != NULL) {
                p += 8;
                size_t i = 0;
                while (*p && *p != '|' && i < sizeof(chat_id_str)-1) chat_id_str[i++] = *p++;
            }
            if ((p = strstr(buf, "name=")) != NULL) {
                p += 5;
                size_t i = 0;
                while (*p && *p != '|' && i < sizeof(name)-1) name[i++] = *p++;
            }
            char notice[BUF_SIZE];
            snprintf(notice, sizeof(notice),
                "Вас добавили в групповой чат \"%s\" (chat_id=%s)", name, chat_id_str);
            ui_set_status(notice);

        } else {
            ui_append_message("server", buf);
        }
    }

    /* Соединение потеряно */
    g_app.connected = 0;
    ui_set_status("Соединение с сервером разорвано");
    return NULL;
}
