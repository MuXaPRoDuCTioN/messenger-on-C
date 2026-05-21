#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>

static sqlite3 *local_db = NULL;

int local_db_init(const char *login) {
    char filename[64];
    snprintf(filename, sizeof(filename), "client_cache_%s.db", login);
    int rc = sqlite3_open(filename, &local_db);
    if (rc) return -1;

    const char *sql =
        "CREATE TABLE IF NOT EXISTS chats ("
        " chat_id INTEGER PRIMARY KEY,"
        " name TEXT,"
        " is_group INTEGER DEFAULT 0);"
        "CREATE TABLE IF NOT EXISTS messages ("
        " msg_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        " chat_id INTEGER NOT NULL,"
        " sender TEXT NOT NULL,"
        " body TEXT NOT NULL,"
        " server_msg_id INTEGER);"
        // Частичный уникальный индекс: запрещает дубли только для реальных ID (>=0)
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_server_msg_id "
        "ON messages(server_msg_id) WHERE server_msg_id >= 0;";
    char *err = NULL;
    rc = sqlite3_exec(local_db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Local DB error: %s\n", err);
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

void local_db_close(void) {
    if (local_db) {
        sqlite3_close(local_db);
        local_db = NULL;
    }
}

void local_db_save_msg(int chat_id, const char *sender, int server_msg_id, const char *body) {
    if (!local_db) return;
    sqlite3_stmt *stmt;
    // Больше не INSERT OR IGNORE, просто INSERT – дубликатов с одинаковыми server_msg_id не будет,
    // потому что от сервера они приходят с уникальными msg_id, а свои имеют -1 (разрешены повторы).
    const char *sql = "INSERT INTO messages (chat_id, sender, body, server_msg_id)"
                      " VALUES (?, ?, ?, ?);";
    sqlite3_prepare_v2(local_db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, chat_id);
    sqlite3_bind_text(stmt, 2, sender, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, body, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, server_msg_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

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

int local_db_get_messages(int chat_id, char ***senders, char ***bodies, int limit) {
    if (!local_db) return 0;
    sqlite3_stmt *stmt;
    const char *sql = "SELECT sender, body FROM messages WHERE chat_id = ? "
                      "ORDER BY msg_id ASC LIMIT ?;";
    sqlite3_prepare_v2(local_db, sql, -1, &stmt, NULL);
    sqlite3_bind_int(stmt, 1, chat_id);
    sqlite3_bind_int(stmt, 2, limit);
    int count = 0;
    char **s = NULL;
    char **b = NULL;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *sender = (const char*)sqlite3_column_text(stmt, 0);
        const char *body = (const char*)sqlite3_column_text(stmt, 1);
        s = realloc(s, (count+1)*sizeof(char*));
        b = realloc(b, (count+1)*sizeof(char*));
        s[count] = strdup(sender ? sender : "unknown");
        b[count] = strdup(body ? body : "");
        count++;
    }
    sqlite3_finalize(stmt);
    *senders = s;
    *bodies = b;
    return count;
}

void local_db_free_chat_list(int count, int *ids, char **names, int *is_groups) {
    free(ids);
    for (int i = 0; i < count; i++) free(names[i]);
    free(names);
    free(is_groups);
}

void local_db_free_messages(int count, char **senders, char **bodies) {
    for (int i = 0; i < count; i++) {
        free(senders[i]);
        free(bodies[i]);
    }
    free(senders);
    free(bodies);
}

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