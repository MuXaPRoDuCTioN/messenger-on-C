#include "client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <ncurses.h>

char my_login[MAX_LOGIN] = {0};

int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");  // поддержка русского языка
    if (argc < 2) {
        printf("Usage: %s <server_ip>\n", argv[0]);
        return 1;
    }

    printf("Введите логин: ");
    fflush(stdout);
    fgets(my_login, sizeof(my_login), stdin);
    my_login[strcspn(my_login, "\n")] = '\0';

    printf("Введите пароль: ");
    fflush(stdout);
    char pass[MAX_PASS];
    fgets(pass, sizeof(pass), stdin);
    pass[strcspn(pass, "\n")] = '\0';

    if (local_db_init(my_login) != 0) {
        fprintf(stderr, "Ошибка инициализации локальной БД\n");
        return 1;
    }

    printf("Подключение к %s:%d...\n", argv[1], DEFAULT_PORT);
    if (connect_to_server(argv[1], DEFAULT_PORT) < 0) {
        fprintf(stderr, "Ошибка подключения\n");
        return 1;
    }
    printf("Подключено!\n");

    char auth_line[MAX_MSG_LINE];
    build_msg(auth_line, sizeof(auth_line), CMD_AUTH,
              "|login=%s|pass=%s", my_login, pass);
    send_cmd(auth_line);
    printf("Отправлен AUTH: %s", auth_line);

    char response[MAX_MSG_LINE];
    int received = recv(sockfd, response, sizeof(response) - 1, 0);
    if (received > 0) {
        response[received] = '\0';
        printf("Ответ сервера: %s\n", response);
        if (strstr(response, "ERR") != NULL) {
            printf("Логин не найден, пробуем регистрацию...\n");
            build_msg(auth_line, sizeof(auth_line), CMD_REG,
                      "|login=%s|pass=%s", my_login, pass);
            send_cmd(auth_line);
            received = recv(sockfd, response, sizeof(response) - 1, 0);
            if (received > 0) {
                response[received] = '\0';
                printf("Ответ регистрации: %s\n", response);
                if (strstr(response, "ERR") != NULL) {
                    printf("Ошибка регистрации!\n");
                    close(sockfd);
                    return 1;
                }
            } else {
                printf("Нет ответа от сервера\n");
                close(sockfd);
                return 1;
            }
        }
        printf("Аутентификация успешна!\n");
    } else {
        printf("Нет ответа от сервера\n");
        close(sockfd);
        return 1;
    }

    printf("Запуск интерфейса...\n");
    sleep(1);

    init_ui();

    int *ids, *is_groups;
    char **names;
    int cnt = local_db_get_chats(&ids, &names, &is_groups);
    for (int i = 0; i < cnt; i++) {
        add_chat(ids[i], names[i], is_groups[i]);
    }
    if (cnt > 0) {
        local_db_free_chat_list(cnt, ids, names, is_groups);
    }

    send_cmd("GET_CHATS\n");
    for (int i = 0; i < chat_count; i++) {
        char req[MAX_MSG_LINE];
        build_msg(req, sizeof(req), CMD_HIST, "|chat_id=%d", chat_list[i].chat_id);
        send_cmd(req);
    }

    redraw_all();

    pthread_t tid;
    pthread_create(&tid, NULL, network_thread, NULL);
    pthread_detach(tid);

    process_input();

    cleanup_ui();
    local_db_close();
    close(sockfd);
    return 0;
}