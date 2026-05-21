#ifndef LOCAL_DB_H
#define LOCAL_DB_H

int local_db_init(const char *login);
void local_db_close(void);
void local_db_save_msg(int chat_id, const char *sender, int server_msg_id, const char *body);
void local_db_add_chat(int id, const char *name, int is_group);
void local_db_confirm_last_msg(int chat_id, int new_server_msg_id);

int local_db_get_chats(int **ids, char ***names, int **is_groups);
int local_db_get_messages(int chat_id, char ***senders, char ***bodies, int limit);
void local_db_free_chat_list(int count, int *ids, char **names, int *is_groups);
void local_db_free_messages(int count, char **senders, char **bodies);

void local_db_update_chat_id(int old_id, int new_id);

#endif