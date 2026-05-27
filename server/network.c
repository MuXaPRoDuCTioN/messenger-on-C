/*
 * server/network.c – сетевые функции сервера:
 *   - логирование событий;
 *   - управление связным списком онлайн-клиентов;
 *   - отправка сообщений (личных, групповых, офлайн);
 *   - получение списка пользователей онлайн.
 */

#include "server.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>

/* ----- глобальный связный список онлайн-клиентов ----- */
client_entry_t *online_head = NULL;               /* голова списка */
pthread_mutex_t online_mutex = PTHREAD_MUTEX_INITIALIZER;   /* мьютекс для защиты списка */

/* ----- для потокобезопасной записи в лог ----- */
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *log_file = NULL;                     /* файл лога (открывается один раз) */

/*
 * server_log – логирует сообщение с временной меткой.
 *
 * Вывод идёт одновременно:
 *   – в файл "server.log" (дописывается в конец);
 *   – в стандартный вывод (консоль).
 *
 * Функция потокобезопасна: использует отдельный мьютекс log_mutex.
 */
void server_log(const char *fmt, ...) {
    pthread_mutex_lock(&log_mutex);

    /* открываем лог-файл при первом вызове */
    if (!log_file) {
        log_file = fopen("server.log", "a");
        if (!log_file) {                     /* не удалось открыть – выходим */
            pthread_mutex_unlock(&log_mutex);
            return;
        }
    }

    /* формируем временную метку */
    time_t now = time(NULL);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));

    /* запись в файл */
    fprintf(log_file, "[%s] ", timestamp);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(log_file, fmt, ap);
    va_end(ap);
    fprintf(log_file, "\n");
    fflush(log_file);

    /* запись в консоль */
    printf("[%s] ", timestamp);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");

    pthread_mutex_unlock(&log_mutex);
}

/*
 * add_online – добавляет клиента в список онлайн.
 *
 * Параметры:
 *   login – логин пользователя;
 *   fd    – файловый дескриптор сокета;
 *   tid   – идентификатор потока, обслуживающего клиента.
 *
 * Заодно логирует факт подключения и текущее количество онлайн-пользователей.
 */
void add_online(const char *login, int fd, pthread_t tid) {
    client_entry_t *e = malloc(sizeof(client_entry_t));
    strncpy(e->login, login, MAX_LOGIN - 1);
    e->login[MAX_LOGIN - 1] = '\0';
    e->fd     = fd;
    e->thread = tid;
    e->login_time = time(NULL);                /* время входа */

    pthread_mutex_lock(&online_mutex);
    e->next = online_head;                     /* вставляем в голову списка */
    online_head = e;

    /* считаем общее количество онлайн */
    int count = 0;
    for (client_entry_t *c = online_head; c; c = c->next) count++;
    pthread_mutex_unlock(&online_mutex);

    server_log("User '%s' connected (fd=%d). Online: %d users", login, fd, count);
}

/*
 * remove_online – удаляет клиента из списка онлайн по файловому дескриптору.
 *
 * Логирует отключение и обновлённое количество онлайн-пользователей.
 */
void remove_online(int fd) {
    char login_removed[MAX_LOGIN] = "unknown";

    pthread_mutex_lock(&online_mutex);
    client_entry_t **p = &online_head;
    while (*p) {
        if ((*p)->fd == fd) {
            client_entry_t *del = *p;
            strncpy(login_removed, del->login, MAX_LOGIN - 1);
            *p = del->next;                    /* вырезаем элемент из списка */
            free(del);
            break;
        }
        p = &(*p)->next;
    }

    /* подсчитываем оставшихся */
    int count = 0;
    for (client_entry_t *c = online_head; c; c = c->next) count++;
    pthread_mutex_unlock(&online_mutex);

    server_log("User '%s' disconnected (fd=%d). Online: %d users",
               login_removed, fd, count);
}

/*
 * find_by_login – ищет клиента в списке онлайн по логину.
 *
 * Возвращает указатель на client_entry_t или NULL, если не найден.
 * Потокобезопасна (сама захватывает и освобождает мьютекс).
 */
client_entry_t *find_by_login(const char *login) {
    pthread_mutex_lock(&online_mutex);
    for (client_entry_t *e = online_head; e; e = e->next)
        if (strcmp(e->login, login) == 0) {
            pthread_mutex_unlock(&online_mutex);
            return e;
        }
    pthread_mutex_unlock(&online_mutex);
    return NULL;
}

/*
 * find_by_fd – ищет клиента в списке онлайн по файловому дескриптору.
 *
 * Аналогична find_by_login, но по fd.
 */
client_entry_t *find_by_fd(int fd) {
    pthread_mutex_lock(&online_mutex);
    for (client_entry_t *e = online_head; e; e = e->next)
        if (e->fd == fd) {
            pthread_mutex_unlock(&online_mutex);
            return e;
        }
    pthread_mutex_unlock(&online_mutex);
    return NULL;
}

/*
 * send_msg – отправляет строку msg в сокет fd.
 *
 * Простая обёртка над send().
 */
void send_msg(int fd, const char *msg) {
    if (fd < 0) return;
    send(fd, msg, strlen(msg), 0);
}

/*
 * broadcast_to_chat – рассылает сообщение всем онлайн-участникам чата.
 *
 * Параметры:
 *   chat_id        – идентификатор чата;
 *   msg            – готовая строка протокола для отправки;
 *   exclude_login  – логин, которому не нужно отправлять (обычно отправитель).
 *
 * Сначала получает список участников чата из БД (db_get_chat_members),
 * затем для каждого участника ищет его в online_head и, если нашёлся,
 * отправляет ему сообщение через send_msg.
 *
 * Мьютекс online_mutex захватывается один раз перед циклом поиска,
 * чтобы избежать deadlock'а (find_by_login не вызывается).
 */
void broadcast_to_chat(int chat_id, const char *msg, const char *exclude_login) {
    char **members;
    int count = db_get_chat_members(chat_id, &members);

    pthread_mutex_lock(&online_mutex);
    for (int i = 0; i < count; i++) {
        /* не шлём отправителю */
        if (exclude_login && strcmp(members[i], exclude_login) == 0) continue;

        /* ручной поиск по списку (мьютекс уже захвачен) */
        client_entry_t *e = NULL;
        for (client_entry_t *cur = online_head; cur; cur = cur->next) {
            if (strcmp(cur->login, members[i]) == 0) {
                e = cur;
                break;
            }
        }
        if (e) send_msg(e->fd, msg);
    }
    pthread_mutex_unlock(&online_mutex);

    db_free_members(members, count);
}

/*
 * send_offline_msg – сохраняет сообщение для получателя, который сейчас не в сети.
 *
 * Данные записываются в таблицу offline_msgs. При следующем входе получателя
 * сервер автоматически доставит все накопленные сообщения.
 */
void send_offline_msg(const char *receiver, int chat_id, const char *sender,
                      const char *body, int msg_id) {
    sqlite3_stmt *stmt;
    const char *sql = "INSERT INTO offline_msgs (receiver, chat_id, sender, body, msg_id)"
                      " VALUES (?, ?, ?, ?, ?);";
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, receiver, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, chat_id);
    sqlite3_bind_text(stmt, 3, sender, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, body, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 5, msg_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    server_log("Offline message saved: %s -> %s (chat %d, msg %d)",
               sender, receiver, chat_id, msg_id);
}

/*
 * get_online_list – формирует строку со списком логинов онлайн-пользователей
 *                   через запятую (для команды LIST).
 *
 * Результирующая строка помещается в buffer (не более size байт).
 * Возвращает количество записанных байт.
 */
int get_online_list(char *buffer, size_t size) {
    int offset = 0;
    pthread_mutex_lock(&online_mutex);
    for (client_entry_t *e = online_head; e; e = e->next) {
        int added = snprintf(buffer + offset, size - offset, "%s%s",
                             e->login, e->next ? "," : "");
        if (added < 0 || (size_t)added >= size - offset) break;
        offset += added;
    }
    pthread_mutex_unlock(&online_mutex);
    return offset;
}

/*
 * get_online_count – возвращает количество пользователей в сети.
 */
int get_online_count(void) {
    int count = 0;
    pthread_mutex_lock(&online_mutex);
    for (client_entry_t *e = online_head; e; e = e->next) count++;
    pthread_mutex_unlock(&online_mutex);
    return count;
}