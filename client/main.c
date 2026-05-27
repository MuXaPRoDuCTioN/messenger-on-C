/*
 * client/main.c – точка входа клиента.
 *
 * Последовательность действий:
 *   1. Запросить у пользователя логин и пароль (с проверкой на ASCII).
 *   2. Инициализировать локальную базу данных (кэш чатов и сообщений).
 *   3. Подключиться к серверу по TCP.
 *   4. Отправить AUTH (или REG, если логин не найден).
 *   5. Запустить графический интерфейс ncurses.
 *   6. Загрузить список чатов из локальной БД и запросить актуальные данные с сервера.
 *   7. Запустить сетевой поток для приёма сообщений.
 *   8. Войти в главный цикл обработки ввода (process_input).
 *   9. При выходе – закрыть соединения и освободить ресурсы.
 */

#include "client.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <ncurses.h>

char my_login[MAX_LOGIN] = {0};   /* глобальная переменная с логином текущего пользователя */

int main(int argc, char *argv[]) {
    setlocale(LC_ALL, "");          /* включаем поддержку UTF-8 для вывода кириллицы */

    /* проверяем аргументы командной строки */
    if (argc < 2) {
        printf("Usage: %s <server_ip>\n", argv[0]);
        return 1;
    }

    /* ---------- ввод логина ---------- */
    printf("Введите логин: ");
    fflush(stdout);
    fgets(my_login, sizeof(my_login), stdin);
    my_login[strcspn(my_login, "\n")] = '\0';   /* убираем символ перевода строки */

    /* запрещаем использовать не-ASCII символы в логине */
    int invalid = 0;
    for (int i = 0; my_login[i]; i++) {
        if ((unsigned char)my_login[i] > 127) {
            invalid = 1;
            break;
        }
    }
    if (invalid) {
        printf("Логин должен содержать только латинские буквы и цифры.\n");
        return 1;
    }

    /* ---------- ввод пароля ---------- */
    printf("Введите пароль: ");
    fflush(stdout);
    char pass[MAX_PASS];
    fgets(pass, sizeof(pass), stdin);
    pass[strcspn(pass, "\n")] = '\0';

    /* ---------- инициализация локальной БД ---------- */
    if (local_db_init(my_login) != 0) {
        fprintf(stderr, "Ошибка инициализации локальной БД\n");
        return 1;
    }

    /* ---------- подключение к серверу ---------- */
    printf("Подключение к %s:%d...\n", argv[1], DEFAULT_PORT);
    if (connect_to_server(argv[1], DEFAULT_PORT) < 0) {
        fprintf(stderr, "Ошибка подключения\n");
        return 1;
    }
    printf("Подключено!\n");

    /* ---------- аутентификация ---------- */
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

        /* если сервер вернул ошибку – пробуем зарегистрироваться */
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
    sleep(1);                       /* небольшая пауза, чтобы пользователь увидел сообщение */

    /* ---------- запуск графического интерфейса (ncurses) ---------- */
    init_ui();

    /* ---------- загружаем список чатов из локального кэша ---------- */
    int *ids, *is_groups;
    char **names;
    int cnt = local_db_get_chats(&ids, &names, &is_groups);
    for (int i = 0; i < cnt; i++) {
        add_chat(ids[i], names[i], is_groups[i]);   /* добавляем каждый чат в левую панель */
    }
    if (cnt > 0) {
        local_db_free_chat_list(cnt, ids, names, is_groups);
    }

    /* ---------- запрашиваем у сервера актуальный список чатов и историю ---------- */
    send_cmd("GET_CHATS\n");
    for (int i = 0; i < chat_count; i++) {
        char req[MAX_MSG_LINE];
        build_msg(req, sizeof(req), CMD_HIST, "|chat_id=%d", chat_list[i].chat_id);
        send_cmd(req);
    }

    redraw_all();                   /* первая полная перерисовка интерфейса */

    /* ---------- запуск сетевого потока для приёма сообщений ---------- */
    pthread_t tid;
    pthread_create(&tid, NULL, network_thread, NULL);
    pthread_detach(tid);            /* поток сам очистит ресурсы при завершении */

    /* ---------- главный цикл обработки ввода ---------- */
    process_input();                /* работает, пока connected == 1 */

    /* ---------- завершение ---------- */
    cleanup_ui();                   /* закрываем ncurses */
    local_db_close();               /* закрываем локальную БД */
    close(sockfd);                  /* закрываем сокет */
    return 0;
}