/*
 * client/local_db.c – локальная база данных клиента (SQLite).
 *
 * Хранит кэш чатов и сообщений в файле client_cache_<login>.db.
 * Это позволяет:
 *   - быстро отображать историю при входе (без запросов к серверу);
 *   - работать с перепиской даже при временном отсутствии сети;
 *   - корректно обрабатывать дубликаты сообщений от сервера.
 *
 * Таблицы:
 *   chats    – список чатов (id, имя, флаг is_group)
 *   messages – сообщения (id, chat_id, отправитель, текст, server_msg_id,
 *              reply_to, fwd_from)
 *
 * Важно: на поле server_msg_id (если >=0) установлен уникальный индекс,
 * чтобы при повторном получении истории с сервера не возникало дубликатов.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>

static sqlite3 *local_db = NULL;   /* глобальный хэндл локальной БД */

/*
 * local_db_init – открывает (или создаёт) локальную базу для пользователя.
 *
 * Параметр login используется для формирования имени файла:
 * client_cache_<login>.db
 *
 * Создаёт таблицы chats и messages, если они ещё не существуют.
 * Возвращает 0 при успехе, -1 при ошибке.
 */
int local_db_init(const char *login) {
    char filename[64];
    snprintf(filename, sizeof(filename), "client_cache_%s.db", login);
    int rc = sqlite3_open(filename, &local_db);
    if (rc) return -1;

    const char *sql =
        "CREATE TABLE IF NOT EXISTS chats ("
        " chat_id INTEGER PRIMARY KEY,"          /* ID чата (может быть -1 до подтверждения сервером) */
        " name TEXT,"                             /* имя собеседника или название группы */
        " is_group INTEGER DEFAULT 0);"           /* 0 – личный, 1 – группа */
        "CREATE TABLE IF NOT EXISTS messages ("
        " msg_id INTEGER PRIMARY KEY AUTOINCREMENT,"  /* локальный автоинкрементный ключ */
        " chat_id INTEGER NOT NULL,"
        " sender TEXT NOT NULL,"
        " body TEXT NOT NULL,"
        " server_msg_id INTEGER,"                     /* ID сообщения на сервере (-1 для своих неподтверждённых) */
        " reply_to INTEGER DEFAULT -1,"               /* ID сообщения, на которое отвечаем */
        " fwd_from INTEGER DEFAULT -1);"              /* ID чата-источника при пересылке */
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_server_msg_id "
        "ON messages(server_msg_id) WHERE server_msg_id >= 0;";   /* не даём дублировать подтверждённые сообщения */

    char *err = NULL;
    rc = sqlite3_exec(local_db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Local DB error: %s\n", err);
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

/*
 * local_db_close – закрывает локальную базу данных.
 */
void local_db_close(void) {
    if (local_db) sqlite3_close(local_db);
}

/*
 * local_db_save_msg – сохраняет сообщение в локальную БД.
 *
 * Параметры:
 *   chat_id       – ID чата
 *   sender        – отправитель
 *   server_msg_id – ID сообщения на сервере (или -1, если ещё не подтверждено)
 *   body          – текст сообщения
 *   reply_to      – ID сообщения, на которое отвечают (-1 если не ответ)
 *   fwd_from      – ID чата, из которого переслано (-1 если не пересылка)
 *
 * Использует INSERT OR IGNORE – если сообщение с таким server_msg_id уже есть,
 * повторно не вставляется (защита от дублирования).
 */
void local_db_save_msg(int chat_id, const char *sender, int server_msg_id, const char *body,
                       int reply_to, int fwd_from) {
    if (!local_db) return;
    sqlite3_stmt *stmt;
    const char *sql = "INSERT OR IGNORE INTO messages (chat_id, sender, body, server_msg_id, reply_to, fwd_from)"
                      " VALUES (?, ?, ?, ?, ?, ?);";
    sqlite3_prepare_v2(local_db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, chat_id);
    sqlite3_bind_text(stmt, 2, sender, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, body, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, server_msg_id);
    sqlite3_bind_int(stmt, 5, reply_to);
    sqlite3_bind_int(stmt, 6, fwd_from);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

/*
 * local_db_add_chat – добавляет (или обновляет) информацию о чате в локальной БД.
 *
 * Если чат с таким chat_id уже существует, он будет обновлён (INSERT OR REPLACE).
 */
void local_db_add_chat(int id, const char *name, int is_group) {
    if (!local_db) return;
    sqlite3_stmt *stmt;
    const char *sql = "INSERT OR REPLACE INTO chats (chat_id, name, is_group) VALUES (?, ?, ?);";
    sqlite3_prepare_v2(local_db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, id);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 3, is_group);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

/*
 * local_db_get_chats – загружает список всех чатов из локальной БД.
 *
 * Возвращает количество чатов, а через параметры – три динамических массива:
 *   ids, names, is_groups
 * Освобождать память нужно вызовом local_db_free_chat_list.
 */
int local_db_get_chats(int **ids, char ***names, int **is_groups) {
    if (!local_db) return 0;
    sqlite3_stmt *stmt;
    const char *sql = "SELECT chat_id, name, is_group FROM chats ORDER BY chat_id;";
    sqlite3_prepare_v2(local_db, sql, -1, &stmt, NULL);
    int count = 0;
    int *id_arr = NULL;
    char **name_arr = NULL;
    int *group_arr = NULL;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt, 0);
        const char *name = (const char*)sqlite3_column_text(stmt, 1);
        int is_group = sqlite3_column_int(stmt, 2);
        id_arr = realloc(id_arr, (count+1)*sizeof(int));
        name_arr = realloc(name_arr, (count+1)*sizeof(char*));
        group_arr = realloc(group_arr, (count+1)*sizeof(int));
        id_arr[count] = id;
        name_arr[count] = strdup(name ? name : "");
        group_arr[count] = is_group;
        count++;
    }
    sqlite3_finalize(stmt);
    *ids = id_arr;
    *names = name_arr;
    *is_groups = group_arr;
    return count;
}

/*
 * local_db_get_messages – загружает сообщения конкретного чата из локальной БД.
 *
 * Параметры:
 *   chat_id – ID чата
 *   limit   – максимальное количество сообщений
 *   senders, bodies, msg_ids, reply_tos, fwd_froms – выходные массивы
 *
 * Возвращает количество сообщений.
 * Освобождать память нужно вызовом local_db_free_messages.
 */
int local_db_get_messages(int chat_id, char ***senders, char ***bodies, int **msg_ids,
                          int **reply_tos, int **fwd_froms, int limit) {
    if (!local_db) return 0;
    sqlite3_stmt *stmt;
    const char *sql = "SELECT sender, body, server_msg_id, reply_to, fwd_from FROM messages WHERE chat_id = ? "
                      "ORDER BY msg_id ASC LIMIT ?;";
    sqlite3_prepare_v2(local_db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, chat_id);
    sqlite3_bind_int(stmt, 2, limit);
    int count = 0;
    char **s = NULL;
    char **b = NULL;
    int *ids = NULL;
    int *r = NULL;
    int *f = NULL;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *sender = (const char*)sqlite3_column_text(stmt, 0);
        const char *body = (const char*)sqlite3_column_text(stmt, 1);
        int server_id = sqlite3_column_int(stmt, 2);
        int reply_to = sqlite3_column_int(stmt, 3);
        int fwd_from = sqlite3_column_int(stmt, 4);
        s = realloc(s, (count+1)*sizeof(char*));
        b = realloc(b, (count+1)*sizeof(char*));
        ids = realloc(ids, (count+1)*sizeof(int));
        r = realloc(r, (count+1)*sizeof(int));
        f = realloc(f, (count+1)*sizeof(int));
        s[count] = strdup(sender ? sender : "unknown");
        b[count] = strdup(body ? body : "");
        ids[count] = server_id;
        r[count] = reply_to;
        f[count] = fwd_from;
        count++;
    }
    sqlite3_finalize(stmt);
    *senders = s;
    *bodies = b;
    *msg_ids = ids;
    *reply_tos = r;
    *fwd_froms = f;
    return count;
}

/*
 * local_db_get_body_by_msg_id – получает текст сообщения по server_msg_id.
 *
 * Возвращает 0 при успехе, -1 если сообщение не найдено.
 */
int local_db_get_body_by_msg_id(int chat_id, int server_msg_id, char *body, size_t size) {
    if (!local_db) return -1;
    sqlite3_stmt *stmt;
    const char *sql = "SELECT body FROM messages WHERE chat_id=? AND server_msg_id=?;";
    sqlite3_prepare_v2(local_db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, chat_id);
    sqlite3_bind_int(stmt, 2, server_msg_id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *text = (const char*)sqlite3_column_text(stmt, 0);
        strncpy(body, text, size-1);
        body[size-1] = '\0';
        sqlite3_finalize(stmt);
        return 0;
    }
    sqlite3_finalize(stmt);
    return -1;
}

/*
 * local_db_get_msg_sender – получает отправителя сообщения по server_msg_id.
 *
 * Возвращает 0 при успехе, -1 если сообщение не найдено.
 */
int local_db_get_msg_sender(int chat_id, int server_msg_id, char *sender, size_t size) {
    if (!local_db) return -1;
    sqlite3_stmt *stmt;
    const char *sql = "SELECT sender FROM messages WHERE chat_id=? AND server_msg_id=?;";
    sqlite3_prepare_v2(local_db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, chat_id);
    sqlite3_bind_int(stmt, 2, server_msg_id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *s = (const char*)sqlite3_column_text(stmt, 0);
        strncpy(sender, s, size-1);
        sender[size-1] = '\0';
        sqlite3_finalize(stmt);
        return 0;
    }
    sqlite3_finalize(stmt);
    return -1;
}

/*
 * local_db_free_chat_list – освобождает память, выделенную в local_db_get_chats.
 */
void local_db_free_chat_list(int count, int *ids, char **names, int *is_groups) {
    free(ids);
    for (int i = 0; i < count; i++) free(names[i]);
    free(names);
    free(is_groups);
}

/*
 * local_db_free_messages – освобождает память, выделенную в local_db_get_messages.
 */
void local_db_free_messages(int count, char **senders, char **bodies, int *msg_ids,
                            int *reply_tos, int *fwd_froms) {
    for (int i = 0; i < count; i++) {
        free(senders[i]);
        free(bodies[i]);
    }
    free(senders);
    free(bodies);
    free(msg_ids);
    free(reply_tos);
    free(fwd_froms);
}

/*
 * local_db_update_chat_id – заменяет старый chat_id на новый во всех связанных записях.
 *
 * Используется, когда сервер вернул реальный chat_id, а клиент до этого использовал
 * временный (например, -1).
 */
void local_db_update_chat_id(int old_id, int new_id) {
    if (!local_db || old_id == new_id) return;
    sqlite3_stmt *stmt;
    const char *sql = "UPDATE OR REPLACE messages SET chat_id = ? WHERE chat_id = ?;";
    sqlite3_prepare_v2(local_db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, new_id);
    sqlite3_bind_int(stmt, 2, old_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    sql = "UPDATE OR REPLACE chats SET chat_id = ? WHERE chat_id = ?;";
    sqlite3_prepare_v2(local_db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, new_id);
    sqlite3_bind_int(stmt, 2, old_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

/*
 * local_db_confirm_last_msg – находит последнее своё сообщение (server_msg_id = -1)
 * в указанном чате и присваивает ему реальный server_msg_id, полученный от сервера.
 *
 * Это позволяет синхронизировать локальный кэш с сервером и избежать повторной вставки
 * того же сообщения при следующей загрузке истории.
 */
void local_db_confirm_last_msg(int chat_id, int new_server_msg_id) {
    if (!local_db) return;
    sqlite3_stmt *stmt;
    const char *sql = "SELECT msg_id FROM messages WHERE chat_id = ? AND server_msg_id = -1 "
                      "ORDER BY msg_id DESC LIMIT 1;";
    sqlite3_prepare_v2(local_db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, chat_id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int local_id = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        sql = "UPDATE messages SET server_msg_id = ? WHERE msg_id = ?;";
        sqlite3_prepare_v2(local_db, sql, -1, &stmt, NULL);
        sqlite3_bind_int(stmt, 1, new_server_msg_id);
        sqlite3_bind_int(stmt, 2, local_id);
        sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
}