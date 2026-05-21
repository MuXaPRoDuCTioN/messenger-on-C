#include "server.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdarg.h>

client_entry_t *online_head = NULL;
pthread_mutex_t online_mutex = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
static FILE *log_file = NULL;

void server_log(const char *fmt, ...) {
    pthread_mutex_lock(&log_mutex);
    if (!log_file) {
        log_file = fopen("server.log", "a");
        if (!log_file) {
            pthread_mutex_unlock(&log_mutex);
            return;
        }
    }
    
    time_t now = time(NULL);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", localtime(&now));
    
    fprintf(log_file, "[%s] ", timestamp);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(log_file, fmt, ap);
    va_end(ap);
    fprintf(log_file, "\n");
    fflush(log_file);
    
    // Дублируем в консоль
    printf("[%s] ", timestamp);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    
    pthread_mutex_unlock(&log_mutex);
}

void add_online(const char *login, int fd, pthread_t tid) {
    client_entry_t *e = malloc(sizeof(client_entry_t));
    strncpy(e->login, login, MAX_LOGIN-1);
    e->login[MAX_LOGIN-1] = '\0';
    e->fd = fd;
    e->thread = tid;
    e->login_time = time(NULL);
    
    pthread_mutex_lock(&online_mutex);
    e->next = online_head;
    online_head = e;
    int count = 0;
    for (client_entry_t *c = online_head; c; c = c->next) count++;
    pthread_mutex_unlock(&online_mutex);
    
    server_log("User '%s' connected (fd=%d). Online: %d users", login, fd, count);
}

void remove_online(int fd) {
    char login_removed[MAX_LOGIN] = "unknown";
    
    pthread_mutex_lock(&online_mutex);
    client_entry_t **p = &online_head;
    while (*p) {
        if ((*p)->fd == fd) {
            client_entry_t *del = *p;
            strncpy(login_removed, del->login, MAX_LOGIN-1);
            *p = del->next;
            free(del);
            break;
        }
        p = &(*p)->next;
    }
    int count = 0;
    for (client_entry_t *c = online_head; c; c = c->next) count++;
    pthread_mutex_unlock(&online_mutex);
    
    server_log("User '%s' disconnected (fd=%d). Online: %d users", login_removed, fd, count);
}

client_entry_t *find_by_login(const char *login) {
    pthread_mutex_lock(&online_mutex);
    for (client_entry_t *e = online_head; e; e = e->next)
        if (strcmp(e->login, login) == 0) {
            pthread_mutex_unlock(&online_mutex);
            return e;
        }
    pthread_mutex_unlock(&online_mutex);
    return NULL;
}

client_entry_t *find_by_fd(int fd) {
    pthread_mutex_lock(&online_mutex);
    for (client_entry_t *e = online_head; e; e = e->next)
        if (e->fd == fd) {
            pthread_mutex_unlock(&online_mutex);
            return e;
        }
    pthread_mutex_unlock(&online_mutex);
    return NULL;
}

void send_msg(int fd, const char *msg) {
    if (fd < 0) return;
    send(fd, msg, strlen(msg), 0);
}

void broadcast_to_chat(int chat_id, const char *msg, const char *exclude_login) {
    char **members;
    int count = db_get_chat_members(chat_id, &members);
    pthread_mutex_lock(&online_mutex);
    int sent = 0;
    for (int i = 0; i < count; i++) {
        if (exclude_login && strcmp(members[i], exclude_login) == 0) continue;
        client_entry_t *e = find_by_login(members[i]);
        if (e) {
            send_msg(e->fd, msg);
            sent++;
        }
    }
    pthread_mutex_unlock(&online_mutex);
    server_log("Broadcast to chat %d: %d online members received message", chat_id, sent);
    db_free_members(members, count);
}

void send_offline_msg(const char *receiver, int chat_id, const char *sender,
                      const char *body, int msg_id) {
    sqlite3_stmt *stmt;
    const char *sql = "INSERT INTO offline_msgs (receiver, chat_id, sender, body, msg_id)"
                      " VALUES (?, ?, ?, ?, ?);";
    sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, receiver, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, chat_id);
    sqlite3_bind_text(stmt, 3, sender, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, body, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 5, msg_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    server_log("Offline message saved: %s -> %s (chat %d, msg %d)", sender, receiver, chat_id, msg_id);
}

int get_online_list(char *buffer, size_t size) {
    int offset = 0;
    pthread_mutex_lock(&online_mutex);
    for (client_entry_t *e = online_head; e; e = e->next) {
        int added = snprintf(buffer + offset, size - offset, "%s%s",
                            e->login, e->next ? "," : "");
        if (added < 0 || (size_t)added >= size - offset) break;
        offset += added;
    }
    pthread_mutex_unlock(&online_mutex);
    return offset;
}

int get_online_count(void) {
    int count = 0;
    pthread_mutex_lock(&online_mutex);
    for (client_entry_t *e = online_head; e; e = e->next) count++;
    pthread_mutex_unlock(&online_mutex);
    return count;
}