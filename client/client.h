#ifndef CLIENT_H
#define CLIENT_H

#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <locale.h>
#include "../common/protocol.h"

#define MAX_CHATS 50
#define MAX_MSG_QUEUE 256

typedef struct {
    char text[MAX_MSG_LINE];
} msg_node_t;

typedef struct {
    msg_node_t *messages;
    int count;
    int capacity;
    pthread_mutex_t mutex;
} msg_queue_t;

typedef struct {
    int chat_id;
    char name[MAX_LOGIN];
    int is_group;
    int msg_count;
    int *server_msg_ids;
    int selected_msg_idx;
    int member_count;   // количество участников (для групп)
} chat_entry_t;

extern int sockfd;
extern volatile int connected;
extern msg_queue_t in_queue;
extern chat_entry_t chat_list[MAX_CHATS];
extern int chat_count;
extern int active_chat_idx;

extern char my_login[MAX_LOGIN];

void init_ui(void);
void cleanup_ui(void);
void redraw_all(void);
void add_chat(int id, const char *name, int is_group);
void process_input(void);

int connect_to_server(const char *ip, int port);
void *network_thread(void *arg);
void send_cmd(const char *buf);

int local_db_init(const char *login);
void local_db_close(void);
void local_db_save_msg(int chat_id, const char *sender, int server_msg_id, const char *body,
                       int reply_to, int fwd_from);
void local_db_add_chat(int id, const char *name, int is_group);
void local_db_confirm_last_msg(int chat_id, int new_server_msg_id);

int local_db_get_chats(int **ids, char ***names, int **is_groups);
int local_db_get_messages(int chat_id, char ***senders, char ***bodies, int **msg_ids,
                          int **reply_tos, int **fwd_froms, int limit);
int local_db_get_body_by_msg_id(int chat_id, int server_msg_id, char *body, size_t size);
int local_db_get_msg_sender(int chat_id, int server_msg_id, char *sender, size_t size);
void local_db_free_chat_list(int count, int *ids, char **names, int *is_groups);
void local_db_free_messages(int count, char **senders, char **bodies, int *msg_ids,
                            int *reply_tos, int *fwd_froms);

void local_db_update_chat_id(int old_id, int new_id);

#endif