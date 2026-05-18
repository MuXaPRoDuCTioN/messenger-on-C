#ifndef CLIENT_H
#define CLIENT_H

#include <pthread.h>
#include "../common/protocol.h"

/* Состояние клиентского приложения */
typedef struct {
    int             server_fd;
    FILE           *stream;       /* только запись — net_send_cmd */
    FILE           *read_stream;  /* только чтение — net_recv_thread */
    char            my_login[MAX_LOGIN];
    int             active_chat_id;    /* Текущий открытый чат */
    int             connected;         /* 1 — онлайн, 0 — оффлайн */
    pthread_mutex_t msg_mutex;         /* Защита очереди входящих */
    pthread_t       net_tid;           /* Сетевой поток */
} AppState;

extern AppState g_app;

#endif /* CLIENT_H */
