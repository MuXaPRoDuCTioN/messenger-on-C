#ifndef SERVER_H
#define SERVER_H

#include <pthread.h>
#include <sqlite3.h>
#include "../common/protocol.h"

/* =========================================================
 * Структуры серверной части
 * ========================================================= */

/* Один подключённый клиент */
typedef struct Client {
    int             fd;                  /* Файловый дескриптор сокета */
    FILE           *stream;             /* Буферизованный поток (fdopen) */
    char            login[MAX_LOGIN];   /* Логин авторизованного пользователя */
    pthread_t       tid;                /* Идентификатор потока-обработчика */
    struct Client  *next;               /* Следующий элемент списка */
} Client;

/* Глобальный список онлайн-клиентов */
extern Client          *g_clients;
extern pthread_mutex_t  g_clients_mutex;

/* Дескриптор базы данных (определён в db.c) */
extern sqlite3         *g_db;
extern pthread_mutex_t  g_db_mutex;

#endif /* SERVER_H */
