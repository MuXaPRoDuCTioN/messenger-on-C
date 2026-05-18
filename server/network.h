#ifndef SERVER_NETWORK_H
#define SERVER_NETWORK_H

#include "server.h"

/* Создать TCP-сокет и начать слушать на указанном порту.
 * Возвращает listening fd или -1 при ошибке. */
int  net_listen(int port);

/* Точка входа потока-обработчика клиента.
 * Аргумент — указатель на Client (выделен через malloc). */
void *net_client_thread(void *arg);

/* Найти клиента по логину (мьютекс должен быть захвачен вызывающим). */
Client *net_find_client(const char *login);

/* Добавить / удалить клиента из глобального списка (захватывают мьютекс сами). */
void net_add_client(Client *c);
void net_remove_client(Client *c);

/* Отправить строку клиенту (захватывает g_clients_mutex). */
void net_send(Client *c, const char *msg);

#endif /* SERVER_NETWORK_H */
