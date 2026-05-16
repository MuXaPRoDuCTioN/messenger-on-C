#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>

#include "client.h"
#include "network.h"
#include "ui.h"

AppState g_app = {
    .server_fd     = -1,
    .stream        = NULL,
    .my_login      = "",
    .active_chat_id = 0,
    .connected     = 0,
    .msg_mutex     = PTHREAD_MUTEX_INITIALIZER,
};

int main(int argc, char *argv[])
{
    const char *host = "127.0.0.1";
    int         port = PORT_DEFAULT;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = atoi(argv[2]);

    if (net_connect(host, port) != 0) {
        fprintf(stderr, "Не удалось подключиться к серверу %s:%d\n", host, port);
        return 1;
    }

    /* TODO (этап 7): ui_init() + ui_run() */
    /* Пока просто сообщение об успехе */
    printf("Подключено к %s:%d\n", host, port);

    return 0;
}
