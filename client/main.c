#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "client.h"
#include "network.h"
#include "ui.h"

AppState g_app = {
    .server_fd      = -1,
    .stream         = NULL,
    .read_stream    = NULL,
    .my_login       = "",
    .active_chat_id = 0,
    .connected      = 0,
    .msg_mutex      = PTHREAD_MUTEX_INITIALIZER,
};

int main(int argc, char *argv[])
{
    const char *host = "127.0.0.1";
    int         port = PORT_DEFAULT;

    if (argc > 1) host = argv[1];
    if (argc > 2) port = atoi(argv[2]);

    printf("Подключение к %s:%d...\n", host, port);

    if (net_connect(host, port) != 0) {
        fprintf(stderr, "Не удалось подключиться к серверу %s:%d\n", host, port);
        return 1;
    }

    printf("Подключено! Вводите команды (Ctrl+D для выхода):\n");
    printf("Примеры: AUTH|login=vasya|pass=123\n"
           "         REG|login=vasya|pass=123\n");

    /* TODO (этап 7): заменить на ui_init() + ui_run() */
    /* Пока — простой консольный ввод для проверки соединения */
    char line[BUF_SIZE];
    while (printf("> "), fflush(stdout), fgets(line, sizeof(line), stdin)) {
        size_t len = strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (strlen(line) == 0) continue;

        net_send_cmd(line);
    }

    printf("\nОтключение.\n");
    if (g_app.stream) fclose(g_app.stream);
    return 0;
}
