#include <stdio.h>
#include <stdlib.h>
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

    /* TODO (этап 3): db_open("server.db"); */

    int listen_fd = net_listen(port);
    if (listen_fd < 0) {
        fprintf(stderr, "Не удалось запустить сервер на порту %d\n", port);
        return 1;
    }
    printf("Сервер запущен на порту %d\n", port);

    /* TODO (этап 2): цикл accept + pthread_create */

    return 0;
}
