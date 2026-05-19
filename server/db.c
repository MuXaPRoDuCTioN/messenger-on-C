#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <pthread.h>

#include "db.h"
#include "server.h"

/* Единственный экземпляр БД — доступ только через g_db_mutex */
sqlite3 *g_db = NULL;

/* =========================================================
 * Вспомогательная функция: выполнить SQL без результата
 * ========================================================= */
static int db_exec(const char *sql)
{
    char *err = NULL;
    int rc = sqlite3_exec(g_db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL ошибка: %s\n", err ? err : "?");
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

/* =========================================================
 * db_open — открыть БД и создать таблицы если их нет
 * ========================================================= */
int db_open(const char *path)
{
    if (sqlite3_open(path, &g_db) != SQLITE_OK) {
        fprintf(stderr, "Не удалось открыть БД: %s\n", sqlite3_errmsg(g_db));
        return -1;
    }

    /* WAL-режим: несколько читателей не блокируют писателя */
    db_exec("PRAGMA journal_mode=WAL;");
    db_exec("PRAGMA foreign_keys=ON;");

    /* Таблица пользователей */
    db_exec(
        "CREATE TABLE IF NOT EXISTS users ("
        "  user_id       INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  login         TEXT    NOT NULL UNIQUE,"
        "  password_hash TEXT    NOT NULL"
        ");"
    );

    /* Таблица чатов */
    db_exec(
        "CREATE TABLE IF NOT EXISTS chats ("
        "  chat_id   INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  chat_name TEXT    NOT NULL,"
        "  is_group  INTEGER NOT NULL DEFAULT 0"
        ");"
    );

    /* Участники групповых чатов */
    db_exec(
        "CREATE TABLE IF NOT EXISTS chat_members ("
        "  chat_id    INTEGER NOT NULL REFERENCES chats(chat_id),"
        "  user_login TEXT    NOT NULL,"
        "  PRIMARY KEY (chat_id, user_login)"
        ");"
    );

    /* Сообщения */
    db_exec(
        "CREATE TABLE IF NOT EXISTS messages ("
        "  msg_id    INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  chat_id   INTEGER NOT NULL REFERENCES chats(chat_id),"
        "  sender    TEXT    NOT NULL,"
        "  body      TEXT    NOT NULL,"
        "  sent_at   INTEGER NOT NULL,"   /* UNIX timestamp */
        "  reply_to  INTEGER DEFAULT 0,"  /* 0 = не цитата */
        "  fwd_from  INTEGER DEFAULT 0,"  /* 0 = не пересылка */
        "  delivered INTEGER NOT NULL DEFAULT 1" /* 0 = ждёт доставки */
        ");"
    );

    printf("База данных открыта: %s\n", path);
    return 0;
}

/* =========================================================
 * db_close
 * ========================================================= */
void db_close(void)
{
    if (g_db) {
        sqlite3_close(g_db);
        g_db = NULL;
    }
}

/* =========================================================
 * db_auth — проверить логин + пароль (хранится как есть,
 * хеширование добавим позже при желании)
 * ========================================================= */
int db_auth(const char *login, const char *pass_hash)
{
    pthread_mutex_lock(&g_db_mutex);

    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT 1 FROM users WHERE login=? AND password_hash=? LIMIT 1;";

    int found = 0;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, login,     -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, pass_hash, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            found = 1;
        sqlite3_finalize(stmt);
    }

    pthread_mutex_unlock(&g_db_mutex);
    return found;
}

/* =========================================================
 * db_register — создать нового пользователя
 * Возвращает 0 при успехе, -1 если логин занят
 * ========================================================= */
int db_register(const char *login, const char *pass_hash)
{
    pthread_mutex_lock(&g_db_mutex);

    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO users (login, password_hash) VALUES (?, ?);";

    int rc = -1;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, login,     -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, pass_hash, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_DONE)
            rc = 0;
        sqlite3_finalize(stmt);
    }

    pthread_mutex_unlock(&g_db_mutex);
    return rc; /* SQLITE_CONSTRAINT при дубликате → rc остаётся -1 */
}

/* =========================================================
 * db_save_message — сохранить сообщение, вернуть msg_id
 * ========================================================= */
long long db_save_message(int chat_id, const char *sender,
                          const char *body,
                          long long reply_to, int fwd_from)
{
    pthread_mutex_lock(&g_db_mutex);

    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO messages (chat_id, sender, body, sent_at, reply_to, fwd_from, delivered)"
        " VALUES (?, ?, ?, strftime('%s','now'), ?, ?, 1);";

    long long id = -1;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int (stmt, 1, chat_id);
        sqlite3_bind_text(stmt, 2, sender,   -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, body,     -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 4, reply_to);
        sqlite3_bind_int (stmt, 5, fwd_from);
        if (sqlite3_step(stmt) == SQLITE_DONE)
            id = sqlite3_last_insert_rowid(g_db);
        sqlite3_finalize(stmt);
    }

    pthread_mutex_unlock(&g_db_mutex);
    return id;
}

/* =========================================================
 * db_get_history — последние limit сообщений чата
 * Возвращает строку вида:
 *   HIST|chat_id=1|count=2\nmsg_id=1|sender=vasya|body=Привет|sent_at=...\n...
 * Вызывающий обязан освободить через free()
 * ========================================================= */
char *db_get_history(int chat_id, int limit)
{
    pthread_mutex_lock(&g_db_mutex);

    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT msg_id, sender, body, sent_at, reply_to, fwd_from"
        " FROM messages WHERE chat_id=?"
        " ORDER BY msg_id DESC LIMIT ?;";

    /* Выделяем буфер с запасом */
    size_t buf_size = 16384;
    char  *buf = malloc(buf_size);
    if (!buf) { pthread_mutex_unlock(&g_db_mutex); return NULL; }
    buf[0] = '\0';

    int count = 0;
    /* Временный буфер для строк сообщений (собираем в обратном порядке) */
    char rows[100][512];

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, chat_id);
        sqlite3_bind_int(stmt, 2, limit);
        while (sqlite3_step(stmt) == SQLITE_ROW && count < 100) {
            long long msg_id   = sqlite3_column_int64(stmt, 0);
            const char *sender = (const char *)sqlite3_column_text(stmt, 1);
            const char *body   = (const char *)sqlite3_column_text(stmt, 2);
            long long sent_at  = sqlite3_column_int64(stmt, 3);
            long long reply_to = sqlite3_column_int64(stmt, 4);
            int fwd_from       = sqlite3_column_int(stmt, 5);

            snprintf(rows[count], sizeof(rows[count]),
                "msg_id=%lld|sender=%s|body=%s|sent_at=%lld|reply_to=%lld|fwd_from=%d",
                msg_id, sender ? sender : "",
                body   ? body   : "",
                sent_at, reply_to, fwd_from);
            count++;
        }
        sqlite3_finalize(stmt);
    }

    /* Заголовок */
    snprintf(buf, buf_size, "HIST|chat_id=%d|count=%d\n", chat_id, count);

    /* Сообщения в хронологическом порядке (они пришли DESC) */
    for (int i = count - 1; i >= 0; i--) {
        strncat(buf, rows[i],    buf_size - strlen(buf) - 2);
        strncat(buf, "\n",       buf_size - strlen(buf) - 1);
    }

    pthread_mutex_unlock(&g_db_mutex);
    return buf;
}

/* =========================================================
 * db_create_group
 * ========================================================= */
int db_create_group(const char *name, const char *members[], int count)
{
    pthread_mutex_lock(&g_db_mutex);

    db_exec("BEGIN;");

    sqlite3_stmt *stmt = NULL;
    int chat_id = -1;

    /* Создать запись чата */
    if (sqlite3_prepare_v2(g_db,
            "INSERT INTO chats (chat_name, is_group) VALUES (?, 1);",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_DONE)
            chat_id = (int)sqlite3_last_insert_rowid(g_db);
        sqlite3_finalize(stmt);
    }

    if (chat_id < 0) {
        db_exec("ROLLBACK;");
        pthread_mutex_unlock(&g_db_mutex);
        return -1;
    }

    /* Добавить участников */
    for (int i = 0; i < count; i++) {
        if (sqlite3_prepare_v2(g_db,
                "INSERT OR IGNORE INTO chat_members (chat_id, user_login) VALUES (?, ?);",
                -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int (stmt, 1, chat_id);
            sqlite3_bind_text(stmt, 2, members[i], -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    db_exec("COMMIT;");
    pthread_mutex_unlock(&g_db_mutex);
    return chat_id;
}

/* =========================================================
 * db_get_members
 * ========================================================= */
int db_get_members(int chat_id, char logins[][64], int max_count)
{
    pthread_mutex_lock(&g_db_mutex);

    sqlite3_stmt *stmt = NULL;
    int n = 0;

    if (sqlite3_prepare_v2(g_db,
            "SELECT user_login FROM chat_members WHERE chat_id=?;",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, chat_id);
        while (sqlite3_step(stmt) == SQLITE_ROW && n < max_count) {
            const char *login = (const char *)sqlite3_column_text(stmt, 0);
            if (login)
                strncpy(logins[n++], login, 63);
        }
        sqlite3_finalize(stmt);
    }

    pthread_mutex_unlock(&g_db_mutex);
    return n;
}

/* =========================================================
 * db_get_or_create_dialog — найти или создать личный чат
 * между двумя пользователями. Возвращает chat_id или -1.
 * ========================================================= */
int db_get_or_create_dialog(const char *login_a, const char *login_b)
{
    pthread_mutex_lock(&g_db_mutex);

    /* Ищем существующий личный чат где оба являются участниками */
    sqlite3_stmt *stmt = NULL;
    int chat_id = -1;

    const char *sql_find =
        "SELECT cm1.chat_id FROM chat_members cm1"
        " JOIN chat_members cm2 ON cm1.chat_id = cm2.chat_id"
        " JOIN chats c ON c.chat_id = cm1.chat_id"
        " WHERE cm1.user_login=? AND cm2.user_login=? AND c.is_group=0"
        " LIMIT 1;";

    if (sqlite3_prepare_v2(g_db, sql_find, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, login_a, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, login_b, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW)
            chat_id = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }

    if (chat_id >= 0) {
        pthread_mutex_unlock(&g_db_mutex);
        return chat_id;
    }

    /* Не нашли — создаём новый личный чат */
    char name[MAX_CHAT_NAME];
    snprintf(name, sizeof(name), "%s<->%s", login_a, login_b);

    sqlite3_exec(g_db, "BEGIN;", NULL, NULL, NULL);

    if (sqlite3_prepare_v2(g_db,
            "INSERT INTO chats (chat_name, is_group) VALUES (?, 0);",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_DONE)
            chat_id = (int)sqlite3_last_insert_rowid(g_db);
        sqlite3_finalize(stmt);
    }

    if (chat_id < 0) {
        sqlite3_exec(g_db, "ROLLBACK;", NULL, NULL, NULL);
        pthread_mutex_unlock(&g_db_mutex);
        return -1;
    }

    /* Добавляем обоих участников */
    const char *members[2] = { login_a, login_b };
    for (int i = 0; i < 2; i++) {
        if (sqlite3_prepare_v2(g_db,
                "INSERT OR IGNORE INTO chat_members (chat_id, user_login)"
                " VALUES (?, ?);",
                -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_int (stmt, 1, chat_id);
            sqlite3_bind_text(stmt, 2, members[i], -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    sqlite3_exec(g_db, "COMMIT;", NULL, NULL, NULL);
    pthread_mutex_unlock(&g_db_mutex);
    return chat_id;
}

/* =========================================================
 * db_mark_delivered / db_get_pending
 * ========================================================= */
void db_mark_delivered(const char *login)
{
    pthread_mutex_lock(&g_db_mutex);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db,
            "UPDATE messages SET delivered=1"
            " WHERE delivered=0 AND chat_id IN"
            "   (SELECT chat_id FROM chat_members WHERE user_login=?);",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, login, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
    pthread_mutex_unlock(&g_db_mutex);
}

/* Возвращает недоставленные сообщения для login в том же формате что HIST.
 * Вызывающий обязан освободить через free(). */
char *db_get_pending(const char *login)
{
    pthread_mutex_lock(&g_db_mutex);

    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT m.msg_id, m.chat_id, m.sender, m.body, m.sent_at,"
        "       m.reply_to, m.fwd_from"
        " FROM messages m"
        " JOIN chat_members cm ON cm.chat_id = m.chat_id"
        " WHERE cm.user_login=? AND m.delivered=0 AND m.sender!=?"
        " ORDER BY m.msg_id ASC;";

    size_t buf_size = 16384;
    char  *buf = malloc(buf_size);
    if (!buf) { pthread_mutex_unlock(&g_db_mutex); return NULL; }
    buf[0] = '\0';

    int count = 0;
    char rows[100][600];

    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, login, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, login, -1, SQLITE_STATIC);
        while (sqlite3_step(stmt) == SQLITE_ROW && count < 100) {
            long long msg_id   = sqlite3_column_int64(stmt, 0);
            int       chat_id  = sqlite3_column_int  (stmt, 1);
            const char *sender = (const char *)sqlite3_column_text(stmt, 2);
            const char *body   = (const char *)sqlite3_column_text(stmt, 3);
            long long sent_at  = sqlite3_column_int64(stmt, 4);
            long long reply_to = sqlite3_column_int64(stmt, 5);
            int fwd_from       = sqlite3_column_int  (stmt, 6);

            snprintf(rows[count], sizeof(rows[count]),
                "INCOMING|msg_id=%lld|chat_id=%d|from=%s|body=%s"
                "|sent_at=%lld|reply_to=%lld|fwd_from=%d",
                msg_id, chat_id,
                sender   ? sender : "",
                body     ? body   : "",
                sent_at, reply_to, fwd_from);
            count++;
        }
        sqlite3_finalize(stmt);
    }

    for (int i = 0; i < count; i++) {
        strncat(buf, rows[i], buf_size - strlen(buf) - 2);
        strncat(buf, "\n",    buf_size - strlen(buf) - 1);
    }

    pthread_mutex_unlock(&g_db_mutex);
    return buf;
}
