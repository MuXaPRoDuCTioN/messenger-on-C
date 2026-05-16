#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>

#include "network.h"
#include "server.h"

/* TODO (этап 2): реализация net_listen() */
int net_listen(int port)
{
    (void)port;
    /* Заглушка — вернём -1 пока не реализовано */
    return -1;
}

/* TODO (этап 2): реализация net_client_thread() */
void *net_client_thread(void *arg)
{
    (void)arg;
    return NULL;
}

Client *net_find_client(const char *login)
{
    /* TODO (этап 2) */
    (void)login;
    return NULL;
}

void net_add_client(Client *c)
{
    /* TODO (этап 2) */
    (void)c;
}

void net_remove_client(Client *c)
{
    /* TODO (этап 2) */
    (void)c;
}

void net_send(Client *c, const char *msg)
{
    /* TODO (этап 2) */
    (void)c;
    (void)msg;
}
