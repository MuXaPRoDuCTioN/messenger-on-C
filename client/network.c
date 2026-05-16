#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "network.h"
#include "client.h"

int net_connect(const char *host, int port)
{
    /* TODO (этап 2) */
    (void)host; (void)port;
    return -1;
}

void net_send_cmd(const char *cmd)
{
    /* TODO (этап 2) */
    (void)cmd;
}

void *net_recv_thread(void *arg)
{
    /* TODO (этап 2) */
    (void)arg;
    return NULL;
}
