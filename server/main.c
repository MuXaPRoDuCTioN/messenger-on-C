#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#include "server.h"
#include "network.h"
#include "db.h"

/* Глобальные переменные (объявлены extern в server.h) */
Client          *g_clients       = NULL;
pthread_mutex_t  g_clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t  g_db_mutex      = PTHREAD_MUTEX_INITIALIZER;

int main(int argc, char *argv[])
{
    int port = PORT_DEFAULT;
    if (argc > 1) port = atoi(argv[1]);

    /* Открываем базу данных */
    if (db_open("server.db") != 0) {
        fprintf(stderr, "Ошибка инициализации базы данных\n");
        return 1;
    }

    int listen_fd = net_listen(port);
    if (listen_fd < 0) {
        fprintf(stderr, "Не удалось запустить сервер на порту %d\n", port);
        return 1;
    }
    printf("Сервер запущен на порту %d. Ожидание подключений...\n", port);

    /* Главный цикл: принимаем входящие соединения */
    while (1) {
        struct sockaddr_in cli_addr;
        socklen_t cli_len = sizeof(cli_addr);

        int cli_fd = accept(listen_fd, (struct sockaddr *)&cli_addr, &cli_len);
        if (cli_fd < 0) {
            perror("accept");
            continue;
        }

        printf("Новое подключение: %s:%d\n",
               inet_ntoa(cli_addr.sin_addr),
               ntohs(cli_addr.sin_port));

        /* Выделяем структуру клиента — поток освободит её сам */
        Client *c = calloc(1, sizeof(Client));
        if (!c) {
            close(cli_fd);
            continue;
        }
        c->fd = cli_fd;

        /* Добавляем в список и запускаем поток */
        net_add_client(c);

        pthread_t tid;
        if (pthread_create(&tid, NULL, net_client_thread, c) != 0) {
            perror("pthread_create");
            net_remove_client(c);
            close(cli_fd);
            free(c);
            continue;
        }
        c->tid = tid;
        pthread_detach(tid); /* Не будем делать join — поток сам почистится */
    }

    close(listen_fd);
    return 0;
}
