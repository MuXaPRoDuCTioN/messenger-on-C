#ifndef SERVER_H
#define SERVER_H

#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sqlite3.h>
#include <time.h>
#include "../common/protocol.h"

#define MAX_CLIENTS 100

typedef struct client_entry {
    char login[MAX_LOGIN];
    int fd;
    pthread_t thread;
    time_t login_time;
    struct client_entry *next;
} client_entry_t;

extern client_entry_t *online_head;
extern pthread_mutex_t online_mutex;
extern sqlite3 *db;

// логирование
void server_log(const char *fmt, ...);

// network
void add_online(const char *login, int fd, pthread_t tid);
void remove_online(int fd);
client_entry_t *find_by_login(const char *login);
client_entry_t *find_by_fd(int fd);
void send_msg(int fd, const char *msg);
void broadcast_to_chat(int chat_id, const char *msg, const char *exclude_login);
void send_offline_msg(const char *receiver, int chat_id, const char *sender,
                      const char *body, int msg_id);
int get_online_list(char *buffer, size_t size);
int get_online_count(void);

// db
int db_init(void);
int db_user_exists(const char *login);
int db_check_password(const char *login, const char *pass);
int db_register_user(const char *login, const char *pass);
int db_save_message(int chat_id, const char *sender, const char *body,
                    int reply_to, int fwd_from);
int db_create_group(const char *name, char **members, int count);
int db_get_chat_id_for_users(const char *user1, const char *user2);
int db_get_pending_messages(const char *login, char ***msgs);
void db_free_pending(char **msgs, int count);
int db_get_chat_members(int chat_id, char ***members);
void db_free_members(char **members, int count);
int db_get_chat_history(int chat_id, int limit, char ***out);
void db_free_history(char **msgs, int count);

#endif