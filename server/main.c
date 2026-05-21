#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

static void *client_handler(void *arg);
static int read_line(int fd, char *buf, size_t size);
static int send_line(int fd, const char *msg);

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    
    server_log("Server starting...");
    
    if (db_init() != 0) {
        server_log("Failed to initialize database");
        return 1;
    }
    server_log("Database initialized");

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(DEFAULT_PORT);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(listen_fd); return 1;
    }
    if (listen(listen_fd, 10) < 0) {
        perror("listen"); close(listen_fd); return 1;
    }

    server_log("Server started on port %d, listening...", DEFAULT_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int *client_fd = malloc(sizeof(int));
        *client_fd = accept(listen_fd, (struct sockaddr*)&client_addr, &client_len);
        if (*client_fd < 0) {
            free(client_fd);
            continue;
        }
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, sizeof(client_ip));
        server_log("New connection from %s:%d (fd=%d)", 
                  client_ip, ntohs(client_addr.sin_port), *client_fd);
        
        pthread_t tid;
        pthread_create(&tid, NULL, client_handler, client_fd);
        pthread_detach(tid);
    }

    close(listen_fd);
    sqlite3_close(db);
    return 0;
}

static int read_line(int fd, char *buf, size_t size) {
    size_t i = 0;
    while (i < size - 1) {
        char c;
        int n = recv(fd, &c, 1, 0);
        if (n <= 0) return -1;
        if (c == '\n') {
            buf[i] = '\0';
            return i;
        }
        if (c != '\r') {
            buf[i++] = c;
        }
    }
    buf[i] = '\0';
    return i;
}

static int send_line(int fd, const char *msg) {
    int len = strlen(msg);
    return send(fd, msg, len, 0);
}

static void *client_handler(void *arg) {
    int client_fd = *(int*)arg;
    free(arg);

    char line[MAX_MSG_LINE];
    char login[MAX_LOGIN] = {0};

    server_log("Waiting for authentication on fd=%d", client_fd);

    while (1) {
        int len = read_line(client_fd, line, sizeof(line));
        if (len < 0) {
            server_log("Client fd=%d disconnected before auth", client_fd);
            close(client_fd);
            return NULL;
        }
        
        server_log("RAW received on fd=%d: '%s'", client_fd, line);
        
        char cmd[MAX_CMD_LEN];
        get_cmd(line, cmd, sizeof(cmd));

        if (strcmp(cmd, CMD_AUTH) == 0) {
            char l[MAX_LOGIN], p[MAX_PASS];
            if (get_param(line, "login", l, sizeof(l)) == 0 &&
                get_param(line, "pass", p, sizeof(p)) == 0) {
                server_log("Parsed: login='%s' pass='%s'", l, p);
                if (db_check_password(l, p)) {
                    strcpy(login, l);
                    char response[MAX_MSG_LINE];
                    snprintf(response, sizeof(response), "OK|login=%s\n", l);
                    send_line(client_fd, response);
                    server_log("User '%s' authenticated successfully (fd=%d)", login, client_fd);
                    break;
                } else {
                    server_log("Failed auth attempt for '%s' (fd=%d)", l, client_fd);
                    send_line(client_fd, "ERR|code=2|desc=Wrong credentials\n");
                }
            } else {
                server_log("Failed to parse AUTH params from: '%s'", line);
                send_line(client_fd, "ERR|code=1|desc=Bad format\n");
            }
        } else if (strcmp(cmd, CMD_REG) == 0) {
            char l[MAX_LOGIN], p[MAX_PASS];
            if (get_param(line, "login", l, sizeof(l)) == 0 &&
                get_param(line, "pass", p, sizeof(p)) == 0) {
                if (db_register_user(l, p)) {
                    strcpy(login, l);
                    char response[MAX_MSG_LINE];
                    snprintf(response, sizeof(response), "OK|login=%s\n", l);
                    send_line(client_fd, response);
                    server_log("New user '%s' registered and authenticated (fd=%d)", login, client_fd);
                    break;
                } else {
                    server_log("Registration failed: login '%s' already taken (fd=%d)", l, client_fd);
                    send_line(client_fd, "ERR|code=1|desc=Login taken\n");
                }
            } else {
                send_line(client_fd, "ERR|code=1|desc=Bad format\n");
            }
        } else {
            server_log("Unknown command before auth: '%s'", cmd);
            send_line(client_fd, "ERR|code=1|desc=Need auth first\n");
        }
    }

    if (login[0] == '\0') {
        server_log("Client fd=%d disconnected without auth", client_fd);
        close(client_fd);
        return NULL;
    }

    add_online(login, client_fd, pthread_self());

    char **pending;
    int cnt = db_get_pending_messages(login, &pending);
    if (cnt > 0) {
        server_log("Delivering %d pending messages to '%s'", cnt, login);
        for (int i = 0; i < cnt; i++) {
            send_line(client_fd, pending[i]);
        }
    }
    db_free_pending(pending, cnt);

    server_log("User '%s' ready, entering command loop (fd=%d)", login, client_fd);

    while (1) {
        int len = read_line(client_fd, line, sizeof(line));
        if (len < 0) {
            server_log("User '%s' disconnected", login);
            break;
        }
        
        server_log("Received from '%s': '%s'", login, line);
        
        char cmd[MAX_CMD_LEN];
        get_cmd(line, cmd, sizeof(cmd));

        if (strcmp(cmd, CMD_MSG) == 0) {
            char to[MAX_LOGIN], body[MAX_BODY];
            if (get_param(line, "to", to, sizeof(to)) == 0 &&
                get_param(line, "text", body, sizeof(body)) == 0) {
                server_log("MSG from '%s' to '%s': \"%s\"", login, to, body);
                int chat_id = db_get_chat_id_for_users(login, to);
                int msg_id = db_save_message(chat_id, login, body, 0, 0);
                client_entry_t *recv = find_by_login(to);
                char fwd[MAX_MSG_LINE];
                build_msg(fwd, sizeof(fwd), CMD_MSG,
                          "|from=%s|chat_id=%d|text=%s|msg_id=%d",
                          login, chat_id, body, msg_id);
                if (recv) {
                    send_line(recv->fd, fwd);
                    server_log("Message delivered to '%s' (online)", to);
                } else {
                    send_offline_msg(to, chat_id, login, body, msg_id);
                    server_log("Message saved for offline user '%s'", to);
                }
                char ok[MAX_MSG_LINE];
                snprintf(ok, sizeof(ok), "OK|msg_id=%d\n", msg_id);
                send_line(client_fd, ok);
            }
        } else if (strcmp(cmd, CMD_GRP) == 0) {
            int chat_id;
            char body[MAX_BODY];
            if (get_param_int(line, "chat_id", &chat_id) == 0 &&
                get_param(line, "text", body, sizeof(body)) == 0) {
                server_log("GRP message from '%s' to chat %d: \"%s\"", login, chat_id, body);
                int msg_id = db_save_message(chat_id, login, body, 0, 0);
                char fwd[MAX_MSG_LINE];
                build_msg(fwd, sizeof(fwd), CMD_GRP,
                          "|from=%s|chat_id=%d|text=%s|msg_id=%d",
                          login, chat_id, body, msg_id);
                broadcast_to_chat(chat_id, fwd, login);
                char ok[MAX_MSG_LINE];
                snprintf(ok, sizeof(ok), "OK|msg_id=%d\n", msg_id);
                send_line(client_fd, ok);
            }
        } else if (strcmp(cmd, CMD_REPLY) == 0) {
            char to[MAX_LOGIN], body[MAX_BODY];
            int reply_to;
            if (get_param(line, "to", to, sizeof(to)) == 0 &&
                get_param(line, "text", body, sizeof(body)) == 0 &&
                get_param_int(line, "reply_to", &reply_to) == 0) {
                server_log("REPLY from '%s' to '%s' (reply_to=%d): \"%s\"", 
                          login, to, reply_to, body);
                int chat_id = db_get_chat_id_for_users(login, to);
                int msg_id = db_save_message(chat_id, login, body, reply_to, 0);
                client_entry_t *recv = find_by_login(to);
                char fwd[MAX_MSG_LINE];
                build_msg(fwd, sizeof(fwd), CMD_REPLY,
                          "|from=%s|chat_id=%d|text=%s|msg_id=%d|reply_to=%d",
                          login, chat_id, body, msg_id, reply_to);
                if (recv) send_line(recv->fd, fwd);
                else send_offline_msg(to, chat_id, login, body, msg_id);
                char ok[MAX_MSG_LINE];
                snprintf(ok, sizeof(ok), "OK|msg_id=%d\n", msg_id);
                send_line(client_fd, ok);
            }
        } else if (strcmp(cmd, CMD_FWD) == 0) {
            char to[MAX_LOGIN], body[MAX_BODY];
            int fwd_from;
            if (get_param(line, "to", to, sizeof(to)) == 0 &&
                get_param(line, "text", body, sizeof(body)) == 0 &&
                get_param_int(line, "fwd_from", &fwd_from) == 0) {
                server_log("FWD from '%s' to '%s' (fwd_from=%d): \"%s\"", 
                          login, to, fwd_from, body);
                int chat_id = db_get_chat_id_for_users(login, to);
                int msg_id = db_save_message(chat_id, login, body, 0, fwd_from);
                client_entry_t *recv = find_by_login(to);
                char fwd[MAX_MSG_LINE];
                build_msg(fwd, sizeof(fwd), CMD_FWD,
                          "|from=%s|chat_id=%d|text=%s|msg_id=%d|fwd_from=%d",
                          login, chat_id, body, msg_id, fwd_from);
                if (recv) send_line(recv->fd, fwd);
                else send_offline_msg(to, chat_id, login, body, msg_id);
                char ok[MAX_MSG_LINE];
                snprintf(ok, sizeof(ok), "OK|msg_id=%d\n", msg_id);
                send_line(client_fd, ok);
            }
        } else if (strcmp(cmd, CMD_CREATE) == 0) {
            char name[MAX_LOGIN], members_str[MAX_MSG_LINE];
            if (get_param(line, "name", name, sizeof(name)) == 0 &&
                get_param(line, "members", members_str, sizeof(members_str)) == 0) {
                char *saveptr;
                char *tok = strtok_r(members_str, ",", &saveptr);
                char *marr[50];
                int mcnt = 0;
                while (tok && mcnt < 50) {
                    marr[mcnt++] = tok;
                    tok = strtok_r(NULL, ",", &saveptr);
                }
                server_log("Creating group '%s' with %d members by '%s'", name, mcnt, login);
                int chat_id = db_create_group(name, marr, mcnt);
                if (chat_id > 0) {
                    char ok[MAX_MSG_LINE];
                    snprintf(ok, sizeof(ok), "OK|chat_id=%d\n", chat_id);
                    send_line(client_fd, ok);

                    char note[MAX_MSG_LINE];
                    build_msg(note, sizeof(note), CMD_OK,
                              "|action=group_created|chat_id=%d|name=%s",
                              chat_id, name);
                    for (int i = 0; i < mcnt; i++) {
                        if (strcmp(marr[i], login) == 0) continue;
                        client_entry_t *receiver = find_by_login(marr[i]);
                        if (receiver) {
                            send_line(receiver->fd, note);
                        }
                    }
                    server_log("Group '%s' created and notifications sent", name);
                } else {
                    send_line(client_fd, "ERR|code=1|desc=Create failed\n");
                }
            } else {
                send_line(client_fd, "ERR|code=1|desc=Bad format\n");
            }
        } else if (strcmp(cmd, CMD_HIST) == 0) {
            int chat_id;
            if (get_param_int(line, "chat_id", &chat_id) == 0) {
                server_log("History request from '%s' for chat %d", login, chat_id);
                char **hist;
                int n = db_get_chat_history(chat_id, 30, &hist);
                for (int i = 0; i < n; i++) {
                    send_line(client_fd, hist[i]);
                }
                db_free_history(hist, n);
                char ok[MAX_MSG_LINE];
                snprintf(ok, sizeof(ok), "OK|hist_end=%d\n", chat_id);
                send_line(client_fd, ok);
            }
        } else if (strcmp(cmd, CMD_LIST) == 0) {
            server_log("LIST request from '%s'", login);
            char online_list[MAX_MSG_LINE] = {0};
            get_online_list(online_list, sizeof(online_list));
            int count = get_online_count();
            char response[MAX_MSG_LINE];
            snprintf(response, sizeof(response), "OK|action=list|count=%d|users=%s\n", 
                    count, online_list);
            send_line(client_fd, response);
        } else if (strcmp(cmd, CMD_HELP) == 0) {
            server_log("HELP request from '%s'", login);
            send_line(client_fd, "OK|action=help|text=Commands: /msg /list /help /quit\n");
        } else if (strcmp(cmd, CMD_GET_CHATS) == 0) {
            server_log("GET_CHATS from '%s'", login);
            sqlite3_stmt *stmt;
            const char *sql = 
                "SELECT c.chat_id, c.chat_name, c.is_group "
                "FROM chats c "
                "JOIN chat_members cm ON c.chat_id = cm.chat_id "
                "WHERE cm.user_login = ?";
            sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
            sqlite3_bind_text(stmt, 1, login, -1, SQLITE_STATIC);
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                int chat_id = sqlite3_column_int(stmt, 0);
                const char *chat_name = (const char*)sqlite3_column_text(stmt, 1);
                int is_group = sqlite3_column_int(stmt, 2);
                
                char display_name[MAX_LOGIN] = {0};
                if (is_group) {
                    strncpy(display_name, chat_name ? chat_name : "Group", MAX_LOGIN-1);
                } else {
                    sqlite3_stmt *stmt2;
                    sqlite3_prepare_v2(db, 
                        "SELECT user_login FROM chat_members WHERE chat_id=? AND user_login!=?",
                        -1, &stmt2, NULL);
                    sqlite3_bind_int(stmt2, 1, chat_id);
                    sqlite3_bind_text(stmt2, 2, login, -1, SQLITE_STATIC);
                    if (sqlite3_step(stmt2) == SQLITE_ROW) {
                        strncpy(display_name, (const char*)sqlite3_column_text(stmt2, 0), MAX_LOGIN-1);
                    } else {
                        strcpy(display_name, "unknown");
                    }
                    sqlite3_finalize(stmt2);
                }
                
                char out[MAX_MSG_LINE];
                snprintf(out, sizeof(out), "CHAT|chat_id=%d|name=%s|is_group=%d\n",
                         chat_id, display_name, is_group);
                send_line(client_fd, out);
            }
            sqlite3_finalize(stmt);
            send_line(client_fd, "OK|action=get_chats_done\n");
        } else {
            server_log("Unknown command from '%s': %s", login, cmd);
            send_line(client_fd, "ERR|code=1|desc=Unknown command\n");
        }
    }

    server_log("User '%s' disconnected from command loop", login);
    remove_online(client_fd);
    close(client_fd);
    return NULL;
}