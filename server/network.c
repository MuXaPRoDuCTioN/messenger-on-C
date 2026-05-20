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
#include "db.h"
#include <sqlite3.h>

/* Forward declaration — определение ниже, после handle_command */
static void client_send(Client *c, const char *msg);

/* =========================================================
 * parse_field — извлечь значение поля из строки протокола
 * Формат: ТИП|ключ=значение|ключ=значение
 * Пишет результат в out (размер out_size), возвращает 1 если нашёл
 * ========================================================= */
static int parse_field(const char *msg, const char *key,
                       char *out, size_t out_size)
{
    /* Ищем "key=" в строке */
    char search[64];
    snprintf(search, sizeof(search), "%s=", key);

    const char *pos = strstr(msg, search);
    if (!pos) return 0;

    pos += strlen(search);

    /* Значение заканчивается на '|' или конец строки */
    size_t i = 0;
    while (*pos && *pos != '|' && i < out_size - 1)
        out[i++] = *pos++;
    out[i] = '\0';
    return 1;
}

/* =========================================================
 * handle_command — разобрать одну строку протокола и ответить
 * ========================================================= */
static void handle_command(Client *c, char *buf)
{
    char resp[BUF_SIZE];

    /* Определяем тип команды — всё до первого '|' */
    char type[32] = {0};
    {
        size_t i = 0;
        while (buf[i] && buf[i] != '|' && i < sizeof(type) - 1)
            { type[i] = buf[i]; i++; }
        type[i] = '\0';
    }

    /* ---- AUTH ------------------------------------------- */
    if (strcmp(type, MSG_AUTH) == 0) {
        char login[MAX_LOGIN] = {0};
        char pass [MAX_LOGIN] = {0};

        if (!parse_field(buf, "login", login, sizeof(login)) ||
            !parse_field(buf, "pass",  pass,  sizeof(pass))) {
            snprintf(resp, sizeof(resp),
                "ERR|code=%d|desc=Неверный формат AUTH", ERR_BAD_FORMAT);
            goto send_resp;
        }

        if (!db_auth(login, pass)) {
            snprintf(resp, sizeof(resp),
                "ERR|code=%d|desc=Неверный логин или пароль", ERR_NOT_FOUND);
            goto send_resp;
        }

        strncpy(c->login, login, MAX_LOGIN - 1);
        printf("Авторизован: %s\n", c->login);
        snprintf(resp, sizeof(resp), "OK|login=%s", login);

        /* Отдаём накопившиеся оффлайн-сообщения сразу после входа */
        char *pending = db_get_pending(login);
        if (pending && pending[0]) {
            pthread_mutex_lock(&c->write_mutex);
            fprintf(c->stream, "%s", pending);
            fflush(c->stream);
            pthread_mutex_unlock(&c->write_mutex);
            db_mark_delivered(login);
        }
        free(pending);
        goto send_resp;
    }

    /* ---- REG -------------------------------------------- */
    if (strcmp(type, MSG_REG) == 0) {
        char login[MAX_LOGIN] = {0};
        char pass [MAX_LOGIN] = {0};

        if (!parse_field(buf, "login", login, sizeof(login)) ||
            !parse_field(buf, "pass",  pass,  sizeof(pass))) {
            snprintf(resp, sizeof(resp),
                "ERR|code=%d|desc=Неверный формат REG", ERR_BAD_FORMAT);
            goto send_resp;
        }

        if (db_register(login, pass) != 0) {
            snprintf(resp, sizeof(resp),
                "ERR|code=%d|desc=Логин уже занят", ERR_EXISTS);
            goto send_resp;
        }

        strncpy(c->login, login, MAX_LOGIN - 1);
        printf("Зарегистрирован: %s\n", c->login);
        snprintf(resp, sizeof(resp), "OK|login=%s", login);
        goto send_resp;
    }

    /* ---- Все остальные команды требуют авторизации ------- */
    if (!c->login[0]) {
        snprintf(resp, sizeof(resp),
            "ERR|code=%d|desc=Сначала выполните AUTH или REG", ERR_FORBIDDEN);
        goto send_resp;
    }

    /* ---- MSG — личное сообщение ------------------------- */
    if (strcmp(type, MSG_SEND) == 0) {
        char to  [MAX_LOGIN] = {0};
        char text[MAX_TEXT]  = {0};

        if (!parse_field(buf, "to",   to,   sizeof(to)) ||
            !parse_field(buf, "text", text, sizeof(text))) {
            snprintf(resp, sizeof(resp),
                "ERR|code=%d|desc=Неверный формат MSG", ERR_BAD_FORMAT);
            goto send_resp;
        }

        /* Получаем или создаём личный чат */
        int chat_id = db_get_or_create_dialog(c->login, to);
        if (chat_id < 0) {
            snprintf(resp, sizeof(resp),
                "ERR|code=%d|desc=Не удалось создать диалог", ERR_NOT_FOUND);
            goto send_resp;
        }

        /* Формируем строку для получателя */
        char incoming[BUF_SIZE];
        snprintf(incoming, sizeof(incoming),
            "INCOMING|chat_id=%d|from=%s|body=%s",
            chat_id, c->login, text);

        /* Сохраняем в БД — delivered=1 если получатель онлайн, иначе 0 */
        pthread_mutex_lock(&g_clients_mutex);
        Client *recipient = net_find_client(to);
        if (recipient) {
            Client *r = recipient;
            pthread_mutex_unlock(&g_clients_mutex);
            client_send(r, incoming);
        } else {
            pthread_mutex_unlock(&g_clients_mutex);
        }

        /* Сохраняем в БД (delivered=1 если онлайн, 0 если оффлайн) */
        db_save_message(chat_id, c->login, text, 0, 0);
        if (!recipient) {
            /* Помечаем как недоставленное — обновляем последнее сообщение */
            /* db_save_message уже пишет delivered=1 по умолчанию,
             * для оффлайн переписываем через отдельный UPDATE */
            sqlite3_stmt *upd = NULL;
            if (sqlite3_prepare_v2(g_db,
                    "UPDATE messages SET delivered=0"
                    " WHERE msg_id=(SELECT MAX(msg_id) FROM messages"
                    "               WHERE chat_id=? AND sender=?);",
                    -1, &upd, NULL) == SQLITE_OK) {
                sqlite3_bind_int (upd, 1, chat_id);
                sqlite3_bind_text(upd, 2, c->login, -1, SQLITE_STATIC);
                sqlite3_step(upd);
                sqlite3_finalize(upd);
            }
        }

        snprintf(resp, sizeof(resp), "OK|chat_id=%d", chat_id);
        goto send_resp;
    }

    /* ---- HIST — история сообщений ----------------------- */
    if (strcmp(type, MSG_HIST) == 0) {
        char chat_id_str[32] = {0};
        if (!parse_field(buf, "chat_id", chat_id_str, sizeof(chat_id_str))) {
            snprintf(resp, sizeof(resp),
                "ERR|code=%d|desc=Неверный формат HIST", ERR_BAD_FORMAT);
            goto send_resp;
        }

        int chat_id = atoi(chat_id_str);
        char *history = db_get_history(chat_id, 50);
        if (!history) {
            snprintf(resp, sizeof(resp),
                "ERR|code=%d|desc=Не удалось получить историю", ERR_NOT_FOUND);
            goto send_resp;
        }

        /* История может быть многострочной — отправляем как есть */
        pthread_mutex_lock(&c->write_mutex);
        fprintf(c->stream, "%s", history);
        fflush(c->stream);
        pthread_mutex_unlock(&c->write_mutex);
        free(history);
        return; /* Уже отправили — не идём в send_resp */
    }

    /* ---- CREATE — создать групповой чат ----------------- */
    if (strcmp(type, MSG_CREATE) == 0) {
        char name   [MAX_CHAT_NAME] = {0};
        char members_str[BUF_SIZE]  = {0};

        if (!parse_field(buf, "name",    name,        sizeof(name)) ||
            !parse_field(buf, "members", members_str, sizeof(members_str))) {
            snprintf(resp, sizeof(resp),
                "ERR|code=%d|desc=Неверный формат CREATE", ERR_BAD_FORMAT);
            goto send_resp;
        }

        /* Разбиваем строку "vasya,petya,kolya" на массив логинов */
        /* Создатель добавляется автоматически */
        const char *member_ptrs[MAX_MEMBERS + 1];
        char        member_buf [MAX_MEMBERS][MAX_LOGIN];
        int         count = 0;

        /* Добавляем самого создателя первым */
        strncpy(member_buf[count], c->login, MAX_LOGIN - 1);
        member_ptrs[count] = member_buf[count];
        count++;

        /* Парсим остальных через strtok на копии строки */
        char tmp[BUF_SIZE];
        strncpy(tmp, members_str, sizeof(tmp) - 1);
        char *tok = strtok(tmp, ",");
        while (tok && count < MAX_MEMBERS) {
            /* Не дублируем создателя если он указал себя */
            if (strcmp(tok, c->login) != 0) {
                strncpy(member_buf[count], tok, MAX_LOGIN - 1);
                member_ptrs[count] = member_buf[count];
                count++;
            }
            tok = strtok(NULL, ",");
        }

        int chat_id = db_create_group(name, member_ptrs, count);
        if (chat_id < 0) {
            snprintf(resp, sizeof(resp),
                "ERR|code=%d|desc=Не удалось создать группу", ERR_BAD_FORMAT);
            goto send_resp;
        }

        /* Уведомляем онлайн-участников — собираем Client* под мьютексом */
        char notify[BUF_SIZE];
        snprintf(notify, sizeof(notify),
            "CHATLIST_UPDATE|chat_id=%d|name=%s|is_group=1", chat_id, name);

        Client *notify_clients[MAX_MEMBERS];
        int     notify_count = 0;

        pthread_mutex_lock(&g_clients_mutex);
        for (int i = 0; i < count; i++) {
            Client *m = net_find_client(member_ptrs[i]);
            if (m && m != c)
                notify_clients[notify_count++] = m;
        }
        pthread_mutex_unlock(&g_clients_mutex);

        /* Пишем через write_mutex каждого клиента — безопасно */
        for (int i = 0; i < notify_count; i++)
            client_send(notify_clients[i], notify);

        printf("Создан групповой чат [%d] \"%s\" (%d участников)\n",
               chat_id, name, count);
        snprintf(resp, sizeof(resp),
            "OK|chat_id=%d|name=%s", chat_id, name);
        goto send_resp;
    }

    /* ---- GRP — сообщение в групповой чат ---------------- */
    if (strcmp(type, MSG_GRP) == 0) {
        char chat_id_str[32]    = {0};
        char text       [MAX_TEXT] = {0};

        if (!parse_field(buf, "chat_id", chat_id_str, sizeof(chat_id_str)) ||
            !parse_field(buf, "text",    text,         sizeof(text))) {
            snprintf(resp, sizeof(resp),
                "ERR|code=%d|desc=Неверный формат GRP", ERR_BAD_FORMAT);
            goto send_resp;
        }

        int chat_id = atoi(chat_id_str);

        /* Получаем список участников группы */
        char logins[MAX_MEMBERS][64];
        int  member_count = db_get_members(chat_id, logins, MAX_MEMBERS);

        if (member_count == 0) {
            snprintf(resp, sizeof(resp),
                "ERR|code=%d|desc=Чат не найден или нет участников", ERR_NOT_FOUND);
            goto send_resp;
        }

        /* Сохраняем сообщение в БД */
        long long msg_id = db_save_message(chat_id, c->login, text, 0, 0);

        /* Формируем строку для получателей */
        char incoming[BUF_SIZE];
        snprintf(incoming, sizeof(incoming),
            "INCOMING|msg_id=%lld|chat_id=%d|from=%s|body=%s",
            msg_id, chat_id, c->login, text);

        /* Рассылаем через write_mutex каждого участника */
        Client *grp_clients[MAX_MEMBERS];
        int     grp_count = 0;

        pthread_mutex_lock(&g_clients_mutex);
        for (int i = 0; i < member_count; i++) {
            if (strcmp(logins[i], c->login) == 0) continue;
            Client *m = net_find_client(logins[i]);
            if (m)
                grp_clients[grp_count++] = m;
        }
        pthread_mutex_unlock(&g_clients_mutex);

        int delivered = grp_count;
        for (int i = 0; i < grp_count; i++)
            client_send(grp_clients[i], incoming);

        /* Помечаем недоставленные сообщения (оффлайн-участники) */
        if (delivered < member_count - 1) {
            sqlite3_stmt *upd = NULL;
            if (sqlite3_prepare_v2(g_db,
                    "UPDATE messages SET delivered=0 WHERE msg_id=?;",
                    -1, &upd, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(upd, 1, msg_id);
                sqlite3_step(upd);
                sqlite3_finalize(upd);
            }
        }

        snprintf(resp, sizeof(resp),
            "OK|chat_id=%d|delivered=%d/%d", chat_id, delivered, member_count - 1);
        goto send_resp;
    }

    /* ---- CHATS — список чатов пользователя -------------- */
    if (strcmp(type, "CHATS") == 0) {
        char *list = db_get_user_chats(c->login);
        if (!list) {
            snprintf(resp, sizeof(resp),
                "ERR|code=%d|desc=Не удалось получить список чатов", ERR_BAD_FORMAT);
            goto send_resp;
        }
        pthread_mutex_lock(&c->write_mutex);
        fprintf(c->stream, "%s", list);
        fflush(c->stream);
        pthread_mutex_unlock(&c->write_mutex);
        free(list);
        return;
    }

    /* ---- REPLY — ответ на сообщение с цитатой ----------- */
    if (strcmp(type, MSG_REPLY) == 0) {
        char to         [MAX_LOGIN] = {0};
        char chat_id_str[32]        = {0};
        char text       [MAX_TEXT]  = {0};
        char reply_to_str[32]       = {0};

        /* Может быть личным (to=) или групповым (chat_id=) */
        int is_group = parse_field(buf, "chat_id", chat_id_str, sizeof(chat_id_str));
        int is_private = parse_field(buf, "to", to, sizeof(to));

        if ((!is_group && !is_private) ||
            !parse_field(buf, "text",     text,         sizeof(text)) ||
            !parse_field(buf, "reply_to", reply_to_str, sizeof(reply_to_str))) {
            snprintf(resp, sizeof(resp),
                "ERR|code=%d|desc=Неверный формат REPLY", ERR_BAD_FORMAT);
            goto send_resp;
        }

        long long reply_to = atoll(reply_to_str);
        int chat_id = -1;

        if (is_private) {
            chat_id = db_get_or_create_dialog(c->login, to);
        } else {
            chat_id = atoi(chat_id_str);
        }

        if (chat_id < 0) {
            snprintf(resp, sizeof(resp),
                "ERR|code=%d|desc=Чат не найден", ERR_NOT_FOUND);
            goto send_resp;
        }

        long long msg_id = db_save_message(chat_id, c->login, text, reply_to, 0);

        /* Формируем INCOMING с пометкой reply_to */
        char incoming[BUF_SIZE];
        snprintf(incoming, sizeof(incoming),
            "INCOMING|msg_id=%lld|chat_id=%d|from=%s|body=%s|reply_to=%lld",
            msg_id, chat_id, c->login, text, reply_to);

        /* Рассылаем участникам чата */
        char logins[MAX_MEMBERS][64];
        int  mcount = db_get_members(chat_id, logins, MAX_MEMBERS);

        Client *targets[MAX_MEMBERS];
        int     tcount = 0;
        pthread_mutex_lock(&g_clients_mutex);
        for (int i = 0; i < mcount; i++) {
            if (strcmp(logins[i], c->login) == 0) continue;
            Client *m = net_find_client(logins[i]);
            if (m) targets[tcount++] = m;
        }
        pthread_mutex_unlock(&g_clients_mutex);
        for (int i = 0; i < tcount; i++)
            client_send(targets[i], incoming);

        snprintf(resp, sizeof(resp), "OK|msg_id=%lld|reply_to=%lld", msg_id, reply_to);
        goto send_resp;
    }

    /* ---- FWD — пересылка сообщения ---------------------- */
    if (strcmp(type, MSG_FWD) == 0) {
        char to         [MAX_LOGIN] = {0};
        char chat_id_str[32]        = {0};
        char fwd_msg_str[32]        = {0}; /* id пересылаемого сообщения */
        char text       [MAX_TEXT]  = {0}; /* текст оригинала (клиент передаёт) */

        int is_group   = parse_field(buf, "chat_id", chat_id_str, sizeof(chat_id_str));
        int is_private  = parse_field(buf, "to",      to,          sizeof(to));

        if ((!is_group && !is_private) ||
            !parse_field(buf, "fwd_msg", fwd_msg_str, sizeof(fwd_msg_str)) ||
            !parse_field(buf, "text",    text,         sizeof(text))) {
            snprintf(resp, sizeof(resp),
                "ERR|code=%d|desc=Неверный формат FWD", ERR_BAD_FORMAT);
            goto send_resp;
        }

        int fwd_msg_id = atoi(fwd_msg_str);
        int chat_id = -1;

        if (is_private) {
            chat_id = db_get_or_create_dialog(c->login, to);
        } else {
            chat_id = atoi(chat_id_str);
        }

        if (chat_id < 0) {
            snprintf(resp, sizeof(resp),
                "ERR|code=%d|desc=Чат не найден", ERR_NOT_FOUND);
            goto send_resp;
        }

        /* fwd_from = id исходного сообщения */
        long long msg_id = db_save_message(chat_id, c->login, text, 0, fwd_msg_id);

        char incoming[BUF_SIZE];
        snprintf(incoming, sizeof(incoming),
            "INCOMING|msg_id=%lld|chat_id=%d|from=%s|body=%s|fwd_from=%d",
            msg_id, chat_id, c->login, text, fwd_msg_id);

        char logins[MAX_MEMBERS][64];
        int  mcount = db_get_members(chat_id, logins, MAX_MEMBERS);

        Client *targets[MAX_MEMBERS];
        int     tcount = 0;
        pthread_mutex_lock(&g_clients_mutex);
        for (int i = 0; i < mcount; i++) {
            if (strcmp(logins[i], c->login) == 0) continue;
            Client *m = net_find_client(logins[i]);
            if (m) targets[tcount++] = m;
        }
        pthread_mutex_unlock(&g_clients_mutex);
        for (int i = 0; i < tcount; i++)
            client_send(targets[i], incoming);

        snprintf(resp, sizeof(resp), "OK|msg_id=%lld|fwd_from=%d", msg_id, fwd_msg_id);
        goto send_resp;
    }

    snprintf(resp, sizeof(resp),
        "ERR|code=%d|desc=Неизвестная команда: %s", ERR_BAD_FORMAT, type);

send_resp:
    client_send(c, resp);
}

/* =========================================================
 * client_send — потокобезопасная запись строки клиенту.
 * Берёт write_mutex клиента, поэтому несколько потоков
 * могут безопасно писать одному клиенту одновременно.
 * ========================================================= */
static void client_send(Client *c, const char *msg)
{
    pthread_mutex_lock(&c->write_mutex);
    if (c->stream) {
        fprintf(c->stream, "%s\n", msg);
        fflush(c->stream);
    }
    pthread_mutex_unlock(&c->write_mutex);
}

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
    client_send(c, "OK|info=Добро пожаловать! Ожидается AUTH или REG");

    char buf[BUF_SIZE];
    while (fgets(buf, sizeof(buf), c->stream)) {
        /* Убрать \n в конце */
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
        if (len == 0) continue;

        printf("[%s] -> %s\n", c->login[0] ? c->login : "?", buf);

        handle_command(c, buf);
    }

    printf("Клиент отключился: %s\n", c->login[0] ? c->login : "?");
    net_remove_client(c);
    pthread_mutex_destroy(&c->write_mutex);
    fclose(c->stream);
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
    client_send(c, msg);
}
