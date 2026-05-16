#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"

/* TODO (этап 3): подключить <sqlite3.h> и реализовать все функции */

int db_open(const char *path)
{
    (void)path;
    return -1; /* Заглушка */
}

void db_close(void) { /* TODO */ }

int db_auth(const char *login, const char *pass_hash)
{
    (void)login; (void)pass_hash;
    return 0;
}

int db_register(const char *login, const char *pass_hash)
{
    (void)login; (void)pass_hash;
    return -1;
}

long long db_save_message(int chat_id, const char *sender,
                          const char *body,
                          long long reply_to, int fwd_from)
{
    (void)chat_id; (void)sender; (void)body;
    (void)reply_to; (void)fwd_from;
    return -1;
}

char *db_get_history(int chat_id, int limit)
{
    (void)chat_id; (void)limit;
    return NULL;
}

int db_create_group(const char *name, const char *members[], int count)
{
    (void)name; (void)members; (void)count;
    return -1;
}

int db_get_members(int chat_id, char logins[][64], int max_count)
{
    (void)chat_id; (void)logins; (void)max_count;
    return 0;
}

void db_mark_delivered(const char *login) { (void)login; }

char *db_get_pending(const char *login)
{
    (void)login;
    return NULL;
}
