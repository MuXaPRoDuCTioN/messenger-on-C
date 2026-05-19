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
            fprintf(c->stream, "%s", pending);
            fflush(c->stream);
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

        /* Сохраняем в БД — delivered=1 если получатель онлайн, иначе 0 */
        pthread_mutex_lock(&g_clients_mutex);
        Client *recipient = net_find_client(to);

        /* Формируем строку для отправки получателю */
        char incoming[BUF_SIZE];
        snprintf(incoming, sizeof(incoming),
            "INCOMING|chat_id=%d|from=%s|body=%s",
            chat_id, c->login, text);

        if (recipient) {
            /* Получатель онлайн — отправляем сразу */
            fprintf(recipient->stream, "%s\n", incoming);
            fflush(recipient->stream);
        }
        pthread_mutex_unlock(&g_clients_mutex);

        /* Сохраняем в БД (delivered=1 если онлайн, 0 если оффлайн) */
        db_save_message(chat_id, c->login, text, 0, 0);
        if (!recipient) {
            /* Помечаем как недоставленное — обновляем последнее сообщение */
            /* db_save_message уже пишет delivered=1 по умолчанию,
             * для оффлайн переписываем через отдельный UPDATE */
            pthread_mutex_lock(&g_db_mutex);
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
            pthread_mutex_unlock(&g_db_mutex);
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
        fprintf(c->stream, "%s", history);
        fflush(c->stream);
        free(history);
        return; /* Уже отправили — не идём в send_resp */
    }

    /* TODO (этап 5): GRP, CREATE */
    snprintf(resp, sizeof(resp),
        "ERR|code=%d|desc=Команда %s будет в следующем этапе", ERR_BAD_FORMAT, type);

send_resp:
    fprintf(c->stream, "%s\n", resp);
    fflush(c->stream);
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
    fprintf(c->stream, "OK|info=Добро пожаловать! Ожидается AUTH или REG\n");
    fflush(c->stream);

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
