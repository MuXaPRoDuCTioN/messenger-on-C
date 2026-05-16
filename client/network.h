#ifndef CLIENT_NETWORK_H
#define CLIENT_NETWORK_H

/* Подключиться к серверу host:port.
 * Возвращает 0 при успехе, -1 при ошибке. */
int  net_connect(const char *host, int port);

/* Отправить строку серверу. */
void net_send_cmd(const char *cmd);

/* Точка входа сетевого потока — читает входящие сообщения от сервера. */
void *net_recv_thread(void *arg);

#endif /* CLIENT_NETWORK_H */
