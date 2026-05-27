/*
 * server/main.c – точка входа сервера.
 *
 * Запускает TCP-сервер на порту DEFAULT_PORT (12345),
 * для каждого подключившегося клиента создаёт отдельный поток,
 * в котором происходит:
 *   – аутентификация (AUTH) или регистрация (REG);
 *   – доставка офлайн-сообщений;
 *   – бесконечный цикл приёма и обработки команд протокола.
 *
 * Протокол текстовый: каждая команда – одна строка, оканчивающаяся '\n'.
 * Формат: КОМАНДА|параметр=значение|...\n
 */

#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

/* предварительные объявления вспомогательных функций */
static void *client_handler(void *arg);
static int read_line(int fd, char *buf, size_t size);
static int send_line(int fd, const char *msg);

/*
 * main() – запуск сервера.
 * 1. Инициализация базы данных (таблицы, индексы).
 * 2. Создание слушающего TCP-сокета.
 * 3. Бесконечный цикл приёма подключений (accept).
 * 4. Для каждого подключения – pthread_create(client_handler).
 */
int main(void) {
    /* игнорируем SIGPIPE, чтобы сервер не падал при обрыве соединения */
    signal(SIGPIPE, SIG_IGN);

    server_log("Server starting...");

    /* инициализация (или открытие) базы данных server.db */
    if (db_init() != 0) {
        server_log("Failed to initialize database");
        return 1;
    }
    server_log("Database initialized");

    /* создаём TCP-сокет */
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    /* разрешаем переиспользовать адрес (удобно при перезапуске) */
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* заполняем адресную структуру: слушаем на всех интерфейсах, порт DEFAULT_PORT */
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(DEFAULT_PORT);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(listen_fd); return 1;
    }
    if (listen(listen_fd, 10) < 0) {   /* очередь на подключение – до 10 */
        perror("listen"); close(listen_fd); return 1;
    }
    server_log("Server started on port %d, listening...", DEFAULT_PORT);

    /* ----- главный цикл приёма клиентов ----- */
    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        /* выделяем память под дескриптор клиента (освободится в потоке) */
        int *client_fd = malloc(sizeof(int));
        *client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
        if (*client_fd < 0) {          /* ошибка accept – пропускаем */
            free(client_fd);
            continue;
        }

        /* логируем IP-адрес и порт подключившегося клиента */
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        server_log("New connection from %s:%d (fd=%d)",
                   client_ip, ntohs(client_addr.sin_port), *client_fd);

        /* создаём поток-обработчик, передаём ему client_fd */
        pthread_t tid;
        pthread_create(&tid, NULL, client_handler, client_fd);
        pthread_detach(tid);   /* поток сам очистит ресурсы после завершения */
    }

    /* формально сюда не доходим, но на случай graceful shutdown */
    close(listen_fd);
    sqlite3_close(db);
    return 0;
}

/*
 * read_line – чтение одной строки из сокета.
 * Читает побайтово до '\n' (или до заполнения буфера).
 * Возвращает длину строки без '\n' (и без '\r'), -1 при ошибке/отключении.
 */
static int read_line(int fd, char *buf, size_t size) {
    size_t i = 0;
    while (i < size - 1) {
        char c;
        int n = recv(fd, &c, 1, 0);   /* читаем 1 байт */
        if (n <= 0) return -1;         /* ошибка или отключение клиента */
        if (c == '\n') {               /* конец строки */
            buf[i] = '\0';
            return i;
        }
        if (c != '\r') buf[i++] = c;   /* '\r' пропускаем (для совместимости с Windows) */
    }
    buf[i] = '\0';
    return i;
}

/*
 * send_line – отправка строки в сокет.
 * Просто обёртка над send().
 */
static int send_line(int fd, const char *msg) {
    int len = strlen(msg);
    return send(fd, msg, len, 0);
}

/*
 * client_handler – функция, выполняемая в отдельном потоке для каждого клиента.
 *
 * Этапы работы:
 *   1. Принять команду AUTH или REG, проверить/создать пользователя.
 *   2. Добавить клиента в список онлайн-пользователей.
 *   3. Доставить офлайн-сообщения (если есть).
 *   4. Войти в бесконечный цикл обработки команд протокола.
 *   5. При разрыве соединения – удалить из списка онлайн, закрыть сокет.
 */
static void *client_handler(void *arg) {
    int client_fd = *(int*)arg;
    free(arg);   /* дескриптор скопирован, память больше не нужна */

    char line[MAX_MSG_LINE];
    char login[MAX_LOGIN] = {0};

    server_log("Waiting for authentication on fd=%d", client_fd);

    /* ---------- этап 1: аутентификация ---------- */
    while (1) {
        int len = read_line(client_fd, line, sizeof(line));
        if (len < 0) {
            server_log("Client fd=%d disconnected before auth", client_fd);
            close(client_fd);
            return NULL;
        }
        server_log("RAW received on fd=%d: '%s'", client_fd, line);

        char cmd[MAX_CMD_LEN];
        get_cmd(line, cmd, sizeof(cmd));   /* извлекаем команду */

        if (strcmp(cmd, CMD_AUTH) == 0) {
            /* попытка авторизации */
            char l[MAX_LOGIN], p[MAX_PASS];
            if (get_param(line, "login", l, sizeof(l)) == 0 &&
                get_param(line, "pass", p, sizeof(p)) == 0) {
                if (db_check_password(l, p)) {
                    strcpy(login, l);
                    char response[MAX_MSG_LINE];
                    snprintf(response, sizeof(response), "OK|login=%s\n", l);
                    send_line(client_fd, response);
                    server_log("User '%s' authenticated successfully", login);
                    break;   /* выходим из цикла аутентификации */
                } else {
                    send_line(client_fd, "ERR|code=2|desc=Wrong credentials\n");
                }
            }
        } else if (strcmp(cmd, CMD_REG) == 0) {
            /* попытка регистрации */
            char l[MAX_LOGIN], p[MAX_PASS];
            if (get_param(line, "login", l, sizeof(l)) == 0 &&
                get_param(line, "pass", p, sizeof(p)) == 0) {
                if (db_register_user(l, p)) {
                    strcpy(login, l);
                    char response[MAX_MSG_LINE];
                    snprintf(response, sizeof(response), "OK|login=%s\n", l);
                    send_line(client_fd, response);
                    server_log("New user '%s' registered", login);
                    break;
                } else {
                    send_line(client_fd, "ERR|code=1|desc=Login taken\n");
                }
            }
        } else {
            /* любая другая команда до авторизации – ошибка */
            send_line(client_fd, "ERR|code=1|desc=Need auth first\n");
        }
    }

    /* если после цикла login остался пустым – клиент не прошёл аутентификацию */
    if (login[0] == '\0') { close(client_fd); return NULL; }

    /* ---------- этап 2: регистрация в списке онлайн ---------- */
    add_online(login, client_fd, pthread_self());

    /* ---------- этап 3: доставка офлайн-сообщений ---------- */
    char **pending;
    int cnt = db_get_pending_messages(login, &pending);
    for (int i = 0; i < cnt; i++) {
        send_line(client_fd, pending[i]);
    }
    db_free_pending(pending, cnt);

    server_log("User '%s' ready", login);

    /* ---------- этап 4: главный цикл обработки команд ---------- */
    while (1) {
        int len = read_line(client_fd, line, sizeof(line));
        if (len < 0) {
            server_log("User '%s' disconnected", login);
            break;   /* клиент отключился */
        }
        server_log("Received from '%s': '%s'", login, line);

        char cmd[MAX_CMD_LEN];
        get_cmd(line, cmd, sizeof(cmd));

        /* ----- личное сообщение (MSG) ----- */
        if (strcmp(cmd, CMD_MSG) == 0) {
            char to[MAX_LOGIN], body[MAX_BODY];
            if (get_param(line, "to", to, sizeof(to)) == 0 &&
                get_param(line, "text", body, sizeof(body)) == 0) {
                /* получаем/создаём чат и сохраняем сообщение */
                int chat_id = db_get_chat_id_for_users(login, to);
                int msg_id  = db_save_message(chat_id, login, body, 0, 0);

                /* ищем получателя среди онлайн */
                client_entry_t *recv = find_by_login(to);
                char fwd[MAX_MSG_LINE];
                build_msg(fwd, sizeof(fwd), CMD_MSG,
                          "|from=%s|chat_id=%d|text=%s|msg_id=%d",
                          login, chat_id, body, msg_id);

                if (recv) {
                    send_line(recv->fd, fwd);                  /* доставляем мгновенно */
                } else {
                    send_offline_msg(to, chat_id, login, body, msg_id); /* сохраняем офлайн */
                }

                /* подтверждение отправителю */
                char ok[MAX_MSG_LINE];
                snprintf(ok, sizeof(ok), "OK|msg_id=%d\n", msg_id);
                send_line(client_fd, ok);
            }
        }
        /* ----- групповое сообщение (GRP) ----- */
        else if (strcmp(cmd, CMD_GRP) == 0) {
            int chat_id, reply_to = -1, fwd_from = -1;
            char body[MAX_BODY];
            if (get_param_int(line, "chat_id", &chat_id) == 0 &&
                get_param(line, "text", body, sizeof(body)) == 0) {
                get_param_int(line, "reply_to", &reply_to);
                get_param_int(line, "fwd_from", &fwd_from);

                int msg_id = db_save_message(chat_id, login, body, reply_to, fwd_from);
                char fwd[MAX_MSG_LINE];
                build_msg(fwd, sizeof(fwd), CMD_GRP,
                          "|from=%s|chat_id=%d|text=%s|msg_id=%d|reply_to=%d|fwd_from=%d",
                          login, chat_id, body, msg_id, reply_to, fwd_from);
                broadcast_to_chat(chat_id, fwd, login);   /* всем онлайн-участникам */

                char ok[MAX_MSG_LINE];
                snprintf(ok, sizeof(ok), "OK|msg_id=%d\n", msg_id);
                send_line(client_fd, ok);
            }
        }
        /* ----- ответ (REPLY) ----- */
        else if (strcmp(cmd, CMD_REPLY) == 0) {
            char to[MAX_LOGIN], body[MAX_BODY];
            int reply_to;
            if (get_param(line, "to", to, sizeof(to)) == 0 &&
                get_param(line, "text", body, sizeof(body)) == 0 &&
                get_param_int(line, "reply_to", &reply_to) == 0) {

                int chat_id = db_get_chat_id_for_users(login, to);
                int msg_id  = db_save_message(chat_id, login, body, reply_to, 0);

                client_entry_t *recv = find_by_login(to);
                char fwd[MAX_MSG_LINE];
                build_msg(fwd, sizeof(fwd), CMD_REPLY,
                          "|from=%s|chat_id=%d|text=%s|msg_id=%d|reply_to=%d",
                          login, chat_id, body, msg_id, reply_to);
                if (recv) send_line(recv->fd, fwd);
                else      send_offline_msg(to, chat_id, login, body, msg_id);

                char ok[MAX_MSG_LINE];
                snprintf(ok, sizeof(ok), "OK|msg_id=%d\n", msg_id);
                send_line(client_fd, ok);
            }
        }
        /* ----- пересылка (FWD) ----- */
        else if (strcmp(cmd, CMD_FWD) == 0) {
            char to[MAX_LOGIN], body[MAX_BODY];
            int fwd_from;
            if (get_param(line, "to", to, sizeof(to)) == 0 &&
                get_param(line, "text", body, sizeof(body)) == 0 &&
                get_param_int(line, "fwd_from", &fwd_from) == 0) {

                int chat_id = db_get_chat_id_for_users(login, to);
                int msg_id  = db_save_message(chat_id, login, body, 0, fwd_from);

                client_entry_t *recv = find_by_login(to);
                char fwd[MAX_MSG_LINE];
                build_msg(fwd, sizeof(fwd), CMD_FWD,
                          "|from=%s|chat_id=%d|text=%s|msg_id=%d|fwd_from=%d",
                          login, chat_id, body, msg_id, fwd_from);
                if (recv) send_line(recv->fd, fwd);
                else      send_offline_msg(to, chat_id, login, body, msg_id);

                char ok[MAX_MSG_LINE];
                snprintf(ok, sizeof(ok), "OK|msg_id=%d\n", msg_id);
                send_line(client_fd, ok);
            }
        }
        /* ----- создание группы (CREATE) ----- */
        else if (strcmp(cmd, CMD_CREATE) == 0) {
            char name[MAX_LOGIN], members_str[MAX_MSG_LINE];
            if (get_param(line, "name", name, sizeof(name)) == 0 &&
                get_param(line, "members", members_str, sizeof(members_str)) == 0) {

                /* разбираем список участников через запятую */
                char *saveptr;
                char *tok = strtok_r(members_str, ",", &saveptr);
                char *marr[50];
                int mcnt = 0;
                while (tok && mcnt < 50) {
                    marr[mcnt++] = tok;
                    tok = strtok_r(NULL, ",", &saveptr);
                }

                int chat_id = db_create_group(name, marr, mcnt);
                if (chat_id > 0) {
                    /* подтверждение создателю */
                    char ok[MAX_MSG_LINE];
                    snprintf(ok, sizeof(ok), "OK|chat_id=%d\n", chat_id);
                    send_line(client_fd, ok);

                    /* уведомление остальных участников (кто онлайн) */
                    char note[MAX_MSG_LINE];
                    build_msg(note, sizeof(note), CMD_OK,
                              "|action=group_created|chat_id=%d|name=%s|member_count=%d",
                              chat_id, name, mcnt);
                    for (int i = 0; i < mcnt; i++) {
                        if (strcmp(marr[i], login) == 0) continue;  /* создателя не уведомляем */
                        client_entry_t *recv = find_by_login(marr[i]);
                        if (recv) send_line(recv->fd, note);
                    }
                } else {
                    send_line(client_fd, "ERR|code=1|desc=Create failed\n");
                }
            }
        }
        /* ----- запрос истории (HIST) ----- */
        else if (strcmp(cmd, CMD_HIST) == 0) {
            int chat_id;
            if (get_param_int(line, "chat_id", &chat_id) == 0) {
                char **hist;
                int n = db_get_chat_history(chat_id, 30, &hist);
                for (int i = 0; i < n; i++) {
                    send_line(client_fd, hist[i]);
                }
                db_free_history(hist, n);
                char ok[MAX_MSG_LINE];
                snprintf(ok, sizeof(ok), "OK|hist_end=%d\n", chat_id);
                send_line(client_fd, ok);
            }
        }
        /* ----- список онлайн-пользователей (LIST) ----- */
        else if (strcmp(cmd, CMD_LIST) == 0) {
            char online_list[MAX_MSG_LINE] = {0};
            get_online_list(online_list, sizeof(online_list));
            int count = get_online_count();
            char response[MAX_MSG_LINE];
            snprintf(response, sizeof(response), "OK|action=list|count=%d|users=%s\n",
                     count, online_list);
            send_line(client_fd, response);
        }
        /* ----- запрос списка чатов текущего пользователя (GET_CHATS) ----- */
        else if (strcmp(cmd, CMD_GET_CHATS) == 0) {
            sqlite3_stmt *stmt;
            const char *sql = 
                "SELECT c.chat_id, c.chat_name, c.is_group, "
                "(SELECT COUNT(*) FROM chat_members WHERE chat_id = c.chat_id) as member_count "
                "FROM chats c JOIN chat_members cm ON c.chat_id = cm.chat_id "
                "WHERE cm.user_login = ? GROUP BY c.chat_id";
            sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
            sqlite3_bind_text(stmt, 1, login, -1, SQLITE_STATIC);

            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int chat_id      = sqlite3_column_int(stmt, 0);
                const char *chat_name = (const char*)sqlite3_column_text(stmt, 1);
                int is_group     = sqlite3_column_int(stmt, 2);
                int member_count = sqlite3_column_int(stmt, 3);

                /* для личных чатов вместо имени чата показываем логин собеседника */
                char display_name[MAX_LOGIN] = {0};
                if (is_group) {
                    strncpy(display_name, chat_name ? chat_name : "Group", MAX_LOGIN-1);
                } else {
                    sqlite3_stmt *stmt2;
                    sqlite3_prepare_v2(db, 
                        "SELECT user_login FROM chat_members WHERE chat_id=? AND user_login!=?",
                        -1, &stmt2, NULL);
                    sqlite3_bind_int(stmt2, 1, chat_id);
                    sqlite3_bind_text(stmt2, 2, login, -1, SQLITE_STATIC);
                    if (sqlite3_step(stmt2) == SQLITE_ROW)
                        strncpy(display_name, (const char*)sqlite3_column_text(stmt2, 0), MAX_LOGIN-1);
                    else
                        strcpy(display_name, "unknown");
                    sqlite3_finalize(stmt2);
                }

                char out[MAX_MSG_LINE];
                snprintf(out, sizeof(out), "CHAT|chat_id=%d|name=%s|is_group=%d|member_count=%d\n",
                         chat_id, display_name, is_group, member_count);
                send_line(client_fd, out);
            }
            sqlite3_finalize(stmt);
            send_line(client_fd, "OK|action=get_chats_done\n");
        }
        else {
            /* неизвестная команда */
            send_line(client_fd, "ERR|code=1|desc=Unknown command\n");
        }
    }

    /* ---------- этап 5: отключение ---------- */
    server_log("User '%s' disconnected", login);
    remove_online(client_fd);   /* удаляем из списка онлайн */
    close(client_fd);
    return NULL;
}