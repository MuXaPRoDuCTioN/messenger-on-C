#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <errno.h>

#include "network.h"
#include "server.h"

/* =========================================================
 * net_listen — создать TCP-сокет и начать прослушивание
 * ========================================================= */
int net_listen(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    /* SO_REUSEADDR — чтобы не ждать TIME_WAIT после перезапуска */
    int opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("setsockopt");
        close(fd);
        return -1;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }

    if (listen(fd, 16) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }

    return fd;
}

/* =========================================================
 * net_client_thread — поток обслуживания одного клиента
 * ========================================================= */
void *net_client_thread(void *arg)
{
    Client *c = (Client *)arg;

    /* Буферизованный поток поверх сокета — удобно читать строки через fgets */
    c->stream = fdopen(c->fd, "r+");
    if (!c->stream) {
        perror("fdopen");
        close(c->fd);
        free(c);
        return NULL;
    }

    /* Приветствие клиенту */
    fprintf(c->stream, "OK|info=Добро пожаловать! Ожидается AUTH или REG\n");
    fflush(c->stream);

    char buf[BUF_SIZE];
    while (fgets(buf, sizeof(buf), c->stream)) {
        /* Убрать \n в конце */
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';

        printf("[%s] -> %s\n", c->login[0] ? c->login : "?", buf);

        /* TODO (этап 3): разбор команд AUTH, REG, MSG, GRP, HIST, CREATE */
        /* Пока просто эхо, чтобы убедиться что соединение работает */
        fprintf(c->stream, "OK|echo=%s\n", buf);
        fflush(c->stream);
    }

    printf("Клиент отключился: %s\n", c->login[0] ? c->login : "?");
    net_remove_client(c);
    fclose(c->stream); /* fclose закрывает и fd */
    free(c);
    return NULL;
}

/* =========================================================
 * Управление списком онлайн-клиентов
 * ========================================================= */
Client *net_find_client(const char *login)
{
    /* Вызывать при захваченном g_clients_mutex */
    for (Client *cur = g_clients; cur; cur = cur->next) {
        if (strcmp(cur->login, login) == 0)
            return cur;
    }
    return NULL;
}

void net_add_client(Client *c)
{
    pthread_mutex_lock(&g_clients_mutex);
    c->next   = g_clients;
    g_clients = c;
    pthread_mutex_unlock(&g_clients_mutex);
}

void net_remove_client(Client *c)
{
    pthread_mutex_lock(&g_clients_mutex);
    Client **pp = &g_clients;
    while (*pp && *pp != c)
        pp = &(*pp)->next;
    if (*pp)
        *pp = c->next;
    pthread_mutex_unlock(&g_clients_mutex);
}

void net_send(Client *c, const char *msg)
{
    pthread_mutex_lock(&g_clients_mutex);
    if (c->stream) {
        fprintf(c->stream, "%s\n", msg);
        fflush(c->stream);
    }
    pthread_mutex_unlock(&g_clients_mutex);
}
