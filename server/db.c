/*
 * server/db.c – работа с базой данных сервера.
 *
 * Используется SQLite (встраиваемая СУБД), база хранится в файле server.db.
 *
 * Таблицы:
 *   users         – учётные записи (логин, пароль)
 *   chats         – чаты (id, название, флаг is_group)
 *   messages      – история сообщений (id, chat_id, отправитель, текст, reply_to, fwd_from)
 *   chat_members  – связь "чат – пользователь"
 *   offline_msgs  – временное хранилище для сообщений, адресованных офлайн-пользователям
 */

#include "server.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

sqlite3 *db = NULL;               /* глобальный хэндл базы данных */

/*
 * db_init – открывает (или создаёт) базу server.db и создаёт таблицы,
 *           если они ещё не существуют.
 * Возвращает 0 при успехе, -1 при ошибке.
 */
int db_init(void) {
    int rc = sqlite3_open("server.db", &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    /* SQL-запрос на создание всех таблиц (CREATE TABLE IF NOT EXISTS) */
    const char *sql =
        "CREATE TABLE IF NOT EXISTS users ("
        " user_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " login TEXT UNIQUE NOT NULL,"
        " password_hash TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS chats ("
        " chat_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " chat_name TEXT,"
        " is_group INTEGER DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS messages ("
        " msg_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " chat_id INTEGER NOT NULL,"
        " sender TEXT NOT NULL,"
        " body TEXT NOT NULL,"
        " sent_at INTEGER DEFAULT (strftime('%s','now')),"
        " reply_to INTEGER DEFAULT 0,"
        " fwd_from INTEGER DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS chat_members ("
        " chat_id INTEGER NOT NULL,"
        " user_login TEXT NOT NULL,"
        " PRIMARY KEY (chat_id, user_login));"
        "CREATE TABLE IF NOT EXISTS offline_msgs ("
        " id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " receiver TEXT NOT NULL,"
        " chat_id INTEGER NOT NULL,"
        " sender TEXT NOT NULL,"
        " body TEXT NOT NULL,"
        " msg_id INTEGER DEFAULT 0);";

    char *errmsg = NULL;
    rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "DB init error: %s\n", errmsg);
        sqlite3_free(errmsg);
        return -1;
    }
    return 0;
}

/*
 * db_user_exists – проверяет, существует ли пользователь с заданным логином.
 * Возвращает 1 если существует, 0 если нет.
 */
int db_user_exists(const char *login) {
    sqlite3_stmt *stmt;
    const char *sql = "SELECT 1 FROM users WHERE login = ?;";
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, login, -1, SQLITE_STATIC);
    int exists = (sqlite3_step(stmt) == SQLITE_ROW) ? 1 : 0;
    sqlite3_finalize(stmt);
    return exists;
}

/*
 * db_check_password – проверяет, совпадает ли пароль с сохранённым для данного логина.
 * Возвращает 1 если пароль верен, 0 если неверен или пользователь не найден.
 * (Пароли сравниваются в открытом виде – в учебных целях.)
 */
int db_check_password(const char *login, const char *pass) {
    sqlite3_stmt *stmt;
    const char *sql = "SELECT password_hash FROM users WHERE login = ?;";
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, login, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *hash = (const char *)sqlite3_column_text(stmt, 0);
        int ok = (strcmp(hash, pass) == 0);
        sqlite3_finalize(stmt);
        return ok;
    }
    sqlite3_finalize(stmt);
    return 0;
}

/*
 * db_register_user – регистрирует нового пользователя.
 * Если логин уже занят, возвращает 0 (не регистрирует).
 * При успешной регистрации возвращает 1.
 */
int db_register_user(const char *login, const char *pass) {
    if (db_user_exists(login)) return 0;
    sqlite3_stmt *stmt;
    const char *sql = "INSERT INTO users (login, password_hash) VALUES (?, ?);";
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, login, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, pass, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? 1 : 0;
}

/*
 * db_save_message – сохраняет сообщение в таблицу messages.
 * Параметры:
 *   chat_id  – идентификатор чата
 *   sender   – логин отправителя
 *   body     – текст сообщения
 *   reply_to – msg_id сообщения, на которое отвечают (0 если не ответ)
 *   fwd_from – chat_id, из которого переслано (0 если не пересылка)
 * Возвращает msg_id сохранённого сообщения или -1 при ошибке.
 */
int db_save_message(int chat_id, const char *sender, const char *body,
                    int reply_to, int fwd_from) {
    sqlite3_stmt *stmt;
    const char *sql = "INSERT INTO messages (chat_id, sender, body, reply_to, fwd_from)"
                      " VALUES (?, ?, ?, ?, ?);";
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, chat_id);
    sqlite3_bind_text(stmt, 2, sender, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, body, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, reply_to);
    sqlite3_bind_int(stmt, 5, fwd_from);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return -1;
    }
    int id = (int)sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);
    return id;
}

/*
 * db_get_chat_id_for_users – возвращает chat_id личного чата между двумя пользователями.
 * Если чата ещё нет – создаёт его (внутри транзакции) и добавляет обоих участников.
 */
int db_get_chat_id_for_users(const char *user1, const char *user2) {
    sqlite3_stmt *stmt;
    /* Ищем существующий личный чат ровно с двумя участниками */
    const char *sql =
        "SELECT c.chat_id FROM chats c"
        " JOIN chat_members m1 ON c.chat_id = m1.chat_id AND m1.user_login = ?"
        " JOIN chat_members m2 ON c.chat_id = m2.chat_id AND m2.user_login = ?"
        " WHERE c.is_group = 0"
        " AND (SELECT COUNT(*) FROM chat_members WHERE chat_id = c.chat_id) = 2;";
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, user1, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, user2, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        int id = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return id;
    }
    sqlite3_finalize(stmt);

    /* Чат не найден – создаём новый внутри транзакции */
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    int new_id = -1;
    sqlite3_prepare_v2(db, "INSERT INTO chats (chat_name, is_group) VALUES ('', 0);",
                       -1, &stmt, NULL);
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        new_id = (int)sqlite3_last_insert_rowid(db);
        sqlite3_finalize(stmt);
        /* Добавляем обоих участников */
        sqlite3_prepare_v2(db,
            "INSERT INTO chat_members (chat_id, user_login) VALUES (?,?);",
            -1, &stmt, NULL);
        sqlite3_bind_int(stmt, 1, new_id);
        sqlite3_bind_text(stmt, 2, user1, -1, SQLITE_STATIC);
        sqlite3_step(stmt); sqlite3_reset(stmt);
        sqlite3_bind_int(stmt, 1, new_id);
        sqlite3_bind_text(stmt, 2, user2, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    return new_id;
}

/*
 * db_create_group – создаёт групповой чат.
 * Параметры:
 *   name    – название группы
 *   members – массив логинов участников
 *   count   – количество участников
 * Возвращает chat_id новой группы или -1 при ошибке.
 * Операция выполняется в транзакции: если на любом шаге ошибка – откат.
 */
int db_create_group(const char *name, char **members, int count) {
    sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);
    sqlite3_stmt *stmt;
    /* Создаём запись в таблице chats */
    sqlite3_prepare_v2(db, "INSERT INTO chats (chat_name, is_group) VALUES (?, 1);",
                       -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        return -1;
    }
    int chat_id = (int)sqlite3_last_insert_rowid(db);
    sqlite3_finalize(stmt);

    /* Добавляем всех участников */
    sqlite3_prepare_v2(db, "INSERT INTO chat_members (chat_id, user_login) VALUES (?,?);",
                       -1, &stmt, NULL);
    for (int i = 0; i < count; i++) {
        sqlite3_bind_int(stmt, 1, chat_id);
        sqlite3_bind_text(stmt, 2, members[i], -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
            return -1;
        }
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);
    return chat_id;
}

/*
 * db_get_pending_messages – загружает все офлайн-сообщения для пользователя.
 * Каждое сообщение упаковывается в строку протокола (CMD_MSG).
 * После загрузки все записи удаляются из таблицы offline_msgs.
 * Возвращает количество сообщений, а через out – массив строк (caller должен освободить
 * через db_free_pending).
 */
int db_get_pending_messages(const char *login, char ***out) {
    sqlite3_stmt *stmt;
    const char *sql =
        "SELECT sender, body, chat_id, msg_id FROM offline_msgs WHERE receiver = ?;";
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, login, -1, SQLITE_STATIC);

    int count = 0;
    char **msgs = NULL;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *sender  = (const char*)sqlite3_column_text(stmt, 0);
        const char *body    = (const char*)sqlite3_column_text(stmt, 1);
        int chat_id = sqlite3_column_int(stmt, 2);
        int msg_id  = sqlite3_column_int(stmt, 3);

        msgs = realloc(msgs, (count + 1) * sizeof(char*));
        char buf[MAX_MSG_LINE];
        build_msg(buf, sizeof(buf), CMD_MSG,
                  "|from=%s|chat_id=%d|text=%s|msg_id=%d",
                  sender, chat_id, body, msg_id);
        msgs[count] = strdup(buf);
        count++;
    }
    sqlite3_finalize(stmt);

    /* Удаляем доставленные офлайн-сообщения */
    sqlite3_prepare_v2(db, "DELETE FROM offline_msgs WHERE receiver = ?;",
                       -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, login, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    *out = msgs;
    return count;
}

/*
 * db_free_pending – освобождает память, выделенную в db_get_pending_messages.
 */
void db_free_pending(char **msgs, int count) {
    for (int i = 0; i < count; i++) free(msgs[i]);
    free(msgs);
}

/*
 * db_get_chat_members – возвращает массив логинов участников чата.
 * Результат через members, количество – возвращаемое значение.
 * Освобождать память нужно через db_free_members.
 */
int db_get_chat_members(int chat_id, char ***members) {
    sqlite3_stmt *stmt;
    const char *sql = "SELECT user_login FROM chat_members WHERE chat_id = ?;";
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, chat_id);

    int count = 0;
    char **m = NULL;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        m = realloc(m, (count + 1) * sizeof(char*));
        m[count] = strdup((const char*)sqlite3_column_text(stmt, 0));
        count++;
    }
    sqlite3_finalize(stmt);
    *members = m;
    return count;
}

/*
 * db_free_members – освобождает память, выделенную в db_get_chat_members.
 */
void db_free_members(char **members, int count) {
    for (int i = 0; i < count; i++) free(members[i]);
    free(members);
}

/*
 * db_get_chat_history – возвращает последние limit сообщений чата.
 * Каждое сообщение упаковывается в строку формата:
 *   chat_id|sender|msg_id|body\n
 * (удобно для передачи клиенту по сети).
 * Освобождать память нужно через db_free_history.
 */
int db_get_chat_history(int chat_id, int limit, char ***out) {
    sqlite3_stmt *stmt;
    const char *sql =
        "SELECT sender, body, msg_id FROM messages "
        "WHERE chat_id = ? ORDER BY msg_id ASC LIMIT ?;";
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, chat_id);
    sqlite3_bind_int(stmt, 2, limit);

    int count = 0;
    char **msgs = NULL;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *sender = (const char*)sqlite3_column_text(stmt, 0);
        const char *body   = (const char*)sqlite3_column_text(stmt, 1);
        int msg_id = sqlite3_column_int(stmt, 2);

        msgs = realloc(msgs, (count + 1) * sizeof(char*));
        char buf[MAX_MSG_LINE];
        snprintf(buf, sizeof(buf), "%d|%s|%d|%s\n", chat_id, sender, msg_id, body);
        msgs[count] = strdup(buf);
        count++;
    }
    sqlite3_finalize(stmt);
    *out = msgs;
    return count;
}

/*
 * db_free_history – освобождает память, выделенную в db_get_chat_history.
 */
void db_free_history(char **msgs, int count) {
    for (int i = 0; i < count; i++) free(msgs[i]);
    free(msgs);
}