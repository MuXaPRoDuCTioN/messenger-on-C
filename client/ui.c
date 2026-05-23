#include "client.h"
#include "local_db.h"

#define NCURSES_WIDECHAR 1
#include <ncurses.h>

#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/select.h>
#include <wchar.h>

chat_entry_t chat_list[MAX_CHATS];
int chat_count = 0;
int active_chat_idx = -1;

static WINDOW *chat_win, *msg_win, *input_win, *help_win;
static int msg_win_h, msg_win_w;
static int chat_win_w;
static int show_help = 0;

static wchar_t input_buf[MAX_BODY];
static int input_len = 0;
static int cur_msg_y = 0;

#define MODE_NORMAL          0
#define MODE_REPLY           1
#define MODE_FWD_SELECT_CHAT 2
#define MODE_FWD_TEXT        3

static int input_mode = MODE_NORMAL;
static int pending_reply_srv_id = 0;
static int pending_reply_local = 0;
static int pending_fwd_srv_id = 0;
static int pending_fwd_local = 0;
static char pending_fwd_user[MAX_LOGIN] = {0};
static char pending_fwd_original_sender[MAX_LOGIN] = {0};
static int fwd_candidate_idx = 0;

enum { COLOR_MY_MSG = 1, COLOR_OTHER_MSG, COLOR_HIGHLIGHT, COLOR_GROUP, COLOR_INFO, COLOR_ID, COLOR_SELECTED };

static void load_chat_history(int chat_id);
static void print_msg_line(const char *sender, const char *body, int local_id, int is_me, int selected,
                           int reply_to_local, int fwd_from_server,
                           const char *reply_sender, const char *fwd_sender);
static void sanitize_body(char *dest, const char *src, size_t dest_size);
static void update_selection(int delta);
static void display_input_line(void);
static int server_id_to_local(const chat_entry_t *chat, int server_id);
static int find_sender_by_server_id(int server_id, char *sender, size_t size);

void init_ui(void) {
    initscr();
    cbreak();
    noecho();
    curs_set(1);
    start_color();
    init_pair(COLOR_MY_MSG, COLOR_GREEN, COLOR_BLACK);
    init_pair(COLOR_OTHER_MSG, COLOR_WHITE, COLOR_BLACK);
    init_pair(COLOR_HIGHLIGHT, COLOR_CYAN, COLOR_BLACK);
    init_pair(COLOR_GROUP, COLOR_YELLOW, COLOR_BLACK);
    init_pair(COLOR_INFO, COLOR_MAGENTA, COLOR_BLACK);
    init_pair(COLOR_ID, COLOR_BLACK, COLOR_WHITE);
    init_pair(COLOR_SELECTED, COLOR_BLACK, COLOR_CYAN);

    int h = LINES - 2, w = COLS;
    chat_win_w = w / 4;
    if (chat_win_w < 12) chat_win_w = 12;
    chat_win = newwin(h, chat_win_w, 0, 0);
    msg_win = newwin(h, w - chat_win_w, 0, chat_win_w);
    input_win = newwin(1, w, LINES-2, 0);
    help_win = newwin(1, w, LINES-1, 0);
    keypad(input_win, TRUE);
    nodelay(input_win, TRUE);
    scrollok(msg_win, TRUE);
    msg_win_h = h;
    msg_win_w = w - chat_win_w;

    wattron(help_win, COLOR_PAIR(COLOR_INFO));
    mvwprintw(help_win, 0, 0, "Enter: отправить | /msg /create | Ctrl+R ответ | Ctrl+F переслать");
    wattroff(help_win, COLOR_PAIR(COLOR_INFO));

    redraw_all();
}

void cleanup_ui(void) {
    delwin(chat_win);
    delwin(msg_win);
    delwin(input_win);
    delwin(help_win);
    endwin();
}

void redraw_all(void) {
    werase(chat_win);
    box(chat_win, 0, 0);
    werase(input_win);
    werase(help_win);

    mvwprintw(chat_win, 0, 1, "Чаты (%d)", chat_count);
    for (int i = 0; i < chat_count; i++) {
        int y = i + 1;
        if (y >= msg_win_h) break;
        if (input_mode == MODE_FWD_SELECT_CHAT && i == fwd_candidate_idx) {
            wattron(chat_win, COLOR_PAIR(COLOR_SELECTED));
        } else if (i == active_chat_idx)
            wattron(chat_win, COLOR_PAIR(COLOR_HIGHLIGHT));
        else if (chat_list[i].is_group)
            wattron(chat_win, COLOR_PAIR(COLOR_GROUP));
        if (chat_list[i].is_group)
            mvwprintw(chat_win, y, 1, "%s(%d)", chat_list[i].name, chat_list[i].member_count);
        else
            mvwprintw(chat_win, y, 1, "%s", chat_list[i].name);
        if (input_mode == MODE_FWD_SELECT_CHAT && i == fwd_candidate_idx)
            wattroff(chat_win, COLOR_PAIR(COLOR_SELECTED));
        else if (i == active_chat_idx)
            wattroff(chat_win, COLOR_PAIR(COLOR_HIGHLIGHT));
        else if (chat_list[i].is_group)
            wattroff(chat_win, COLOR_PAIR(COLOR_GROUP));
    }

    display_input_line();

    wattron(help_win, COLOR_PAIR(COLOR_INFO));
    if (show_help) {
        mvwprintw(help_win, 0, 0, "Enter: отправить | /msg /create | Ctrl+R ответ | Ctrl+F переслать");
    } else if (input_mode == MODE_FWD_SELECT_CHAT) {
        mvwprintw(help_win, 0, 0, "Tab: выбрать чат | Enter: подтвердить выбор");
    } else {
        mvwprintw(help_win, 0, 0, "F1: выход | Tab: чаты | ↑↓: выбрать сообщение");
    }
    wattroff(help_win, COLOR_PAIR(COLOR_INFO));

    wrefresh(chat_win);
    wrefresh(input_win);
    wrefresh(help_win);
}

static void display_input_line(void) {
    werase(input_win);
    if (input_mode == MODE_REPLY) {
        mvwprintw(input_win, 0, 0, ">> ответ на #%d: ", pending_reply_local);
    } else if (input_mode == MODE_FWD_SELECT_CHAT) {
        mvwprintw(input_win, 0, 0, ">> переслать #%d -> %s (Enter для выбора): ",
                  pending_fwd_local,
                  chat_list[fwd_candidate_idx].name);
    } else if (input_mode == MODE_FWD_TEXT) {
        mvwprintw(input_win, 0, 0, ">> переслать #%d -> %s: ",
                  pending_fwd_local, pending_fwd_user);
    } else {
        mvwaddstr(input_win, 0, 0, "> ");
    }
    for (int i = 0; i < input_len; i++) {
        wchar_t wch = input_buf[i];
        char mb[8] = {0};
        if (wctomb(mb, wch) > 0) {
            waddstr(input_win, mb);
        }
    }
    wrefresh(input_win);
}

void add_chat(int id, const char *name, int is_group) {
    for (int i = 0; i < chat_count; i++) {
        if (strcmp(chat_list[i].name, name) == 0 && chat_list[i].is_group == is_group) {
            if (id > 0 && chat_list[i].chat_id != id) {
                local_db_update_chat_id(chat_list[i].chat_id, id);
                chat_list[i].chat_id = id;
                local_db_add_chat(id, name, is_group);
            }
            return;
        }
    }
    if (chat_count < MAX_CHATS) {
        chat_list[chat_count].chat_id = id;
        strncpy(chat_list[chat_count].name, name, MAX_LOGIN-1);
        chat_list[chat_count].name[MAX_LOGIN-1] = '\0';
        chat_list[chat_count].is_group = is_group;
        chat_list[chat_count].msg_count = 0;
        chat_list[chat_count].server_msg_ids = NULL;
        chat_list[chat_count].selected_msg_idx = -1;
        chat_list[chat_count].member_count = 0;
        chat_count++;
        if (active_chat_idx < 0) active_chat_idx = 0;
        local_db_add_chat(id, name, is_group);
    }
}

static void sanitize_body(char *dest, const char *src, size_t dest_size) {
    const char *p = src;
    char *q = dest;
    while (*p && (size_t)(q - dest) < dest_size - 1) {
        if (*p == '|') { *q++ = ';'; p++; }
        else if (*p == '\n') { *q++ = ' '; p++; }
        else *q++ = *p++;
    }
    *q = '\0';
}

void process_command(const char *cmd_line) {
    if (cmd_line[0] != '/') return;
    char cmd_copy[MAX_MSG_LINE];
    strncpy(cmd_copy, cmd_line + 1, MAX_MSG_LINE - 1);
    cmd_copy[MAX_MSG_LINE - 1] = '\0';

    char *saveptr;
    char *cmd = strtok_r(cmd_copy, " ", &saveptr);
    if (!cmd) return;

    if (strcmp(cmd, "msg") == 0) {
        char *user = strtok_r(NULL, " ", &saveptr);
        if (!user) return;
        int found = -1;
        for (int i = 0; i < chat_count; i++) {
            if (strcmp(chat_list[i].name, user) == 0 && !chat_list[i].is_group) {
                found = i; break;
            }
        }
        if (found >= 0) active_chat_idx = found;
        else {
            add_chat(-1, user, 0);
            active_chat_idx = chat_count - 1;
            send_cmd("GET_CHATS\n");
        }
        input_mode = MODE_NORMAL;
        input_len = 0; memset(input_buf, 0, sizeof(input_buf));
        load_chat_history(chat_list[active_chat_idx].chat_id);
        redraw_all();
    } else if (strcmp(cmd, "create") == 0) {
        char *name = strtok_r(NULL, " ", &saveptr);
        char *members = strtok_r(NULL, "", &saveptr);
        if (name && members) {
            char sendline[MAX_MSG_LINE];
            build_msg(sendline, sizeof(sendline), CMD_CREATE,
                      "|name=%s|members=%s", name, members);
            send_cmd(sendline);
        }
    } else if (strcmp(cmd, "list") == 0) {
        send_cmd("LIST\n");
    } else if (strcmp(cmd, "help") == 0) {
        show_help = !show_help;
        redraw_all();
    } else if (strcmp(cmd, "quit") == 0) {
        connected = 0;
    }
}

static void update_selection(int delta) {
    if (active_chat_idx < 0 || chat_list[active_chat_idx].msg_count == 0) return;
    chat_entry_t *chat = &chat_list[active_chat_idx];
    int new_sel = chat->selected_msg_idx + delta;
    if (new_sel < 0) new_sel = chat->msg_count - 1;
    if (new_sel >= chat->msg_count) new_sel = 0;
    chat->selected_msg_idx = new_sel;
    load_chat_history(chat->chat_id);
}

static int server_id_to_local(const chat_entry_t *chat, int server_id) {
    if (!chat->server_msg_ids || server_id < 0) return -1;
    for (int i = 0; i < chat->msg_count; i++) {
        if (chat->server_msg_ids[i] == server_id) return i + 1;
    }
    return -1;
}

static int find_sender_by_server_id(int server_id, char *sender, size_t size) {
    if (server_id < 0) return 0;
    for (int c = 0; c < chat_count; c++) {
        chat_entry_t *chat = &chat_list[c];
        if (!chat->server_msg_ids) continue;
        for (int i = 0; i < chat->msg_count; i++) {
            if (chat->server_msg_ids[i] == server_id) {
                if (local_db_get_msg_sender(chat->chat_id, server_id, sender, size) == 0) {
                    return 1;
                }
                return 0;
            }
        }
    }
    return 0;
}

static void extract_fwd_sender(const char *body, char *sender, size_t size) {
    if (!body || !sender || size == 0) return;
    const char *p = body;
    if (strncmp(p, "[from ", 6) == 0) {
        p += 6;
        const char *end = strchr(p, ']');
        if (end) {
            size_t len = end - p;
            if (len >= size) len = size - 1;
            memcpy(sender, p, len);
            sender[len] = '\0';
            return;
        }
    }
    snprintf(sender, size, "unknown");
}

static void print_msg_line(const char *sender, const char *body, int local_id, int is_me, int selected,
                           int reply_to_local, int fwd_from_server,
                           const char *reply_sender, const char *fwd_sender) {
    char clean_body[MAX_BODY];
    const char *src = body;
    char *dst = clean_body;
    while (*src && (dst - clean_body) < (int)(sizeof(clean_body)-1)) {
        if (*src == '\n') *dst++ = ' ';
        else *dst++ = *src++;
    }
    *dst = '\0';

    // Если сообщение является пересланным и отправитель известен, убираем [from ...] из тела
    if (fwd_from_server >= 0 && fwd_sender && fwd_sender[0]) {
        char *from_pos = strstr(clean_body, "[from ");
        if (from_pos) {
            char *end_pos = strchr(from_pos, ']');
            if (end_pos) {
                // убираем "[from ...] " (включая следующий пробел, если есть)
                int len = end_pos - from_pos + 1; // длина "[from ...]"
                if (*(end_pos+1) == ' ') len++; // учитываем пробел
                memmove(from_pos, from_pos + len, strlen(from_pos + len) + 1);
            }
        }
    }

    if (cur_msg_y >= msg_win_h - 1) {
        scroll(msg_win);
        cur_msg_y = msg_win_h - 2;
    }
    if (selected) {
        wattron(msg_win, COLOR_PAIR(COLOR_SELECTED));
    } else {
        wattron(msg_win, COLOR_PAIR(COLOR_ID));
    }
    mvwprintw(msg_win, ++cur_msg_y, 1, "[%d]", local_id);
    if (selected) wattroff(msg_win, COLOR_PAIR(COLOR_SELECTED));
    else wattroff(msg_win, COLOR_PAIR(COLOR_ID));

    char prefix[64] = "";
    if (reply_to_local > 0 && reply_sender) {
        snprintf(prefix, sizeof(prefix), "(ответ на #%d от %s) ", reply_to_local, reply_sender);
    } else if (reply_to_local > 0) {
        snprintf(prefix, sizeof(prefix), "(ответ на #%d) ", reply_to_local);
    } else if (fwd_from_server >= 0) {
        if (fwd_sender && fwd_sender[0]) {
            snprintf(prefix, sizeof(prefix), "(от %s) ", fwd_sender);
        } else {
            // fallback: попытка извлечь из тела, если отправитель не найден
            char fwd_sender_buf[MAX_LOGIN];
            extract_fwd_sender(clean_body, fwd_sender_buf, sizeof(fwd_sender_buf));
            if (fwd_sender_buf[0] && strcmp(fwd_sender_buf, "unknown") != 0) {
                snprintf(prefix, sizeof(prefix), "(от %s) ", fwd_sender_buf);
                // Удалим [from ...] из тела, чтобы не дублировать
                char *from_pos2 = strstr(clean_body, "[from ");
                if (from_pos2) {
                    char *end_pos2 = strchr(from_pos2, ']');
                    if (end_pos2) {
                        int len = end_pos2 - from_pos2 + 1;
                        if (*(end_pos2+1) == ' ') len++;
                        memmove(from_pos2, from_pos2 + len, strlen(from_pos2 + len) + 1);
                    }
                }
            } else {
                snprintf(prefix, sizeof(prefix), "(переслано) ");
            }
        }
    }

    if (is_me) wattron(msg_win, COLOR_PAIR(COLOR_MY_MSG));
    else wattron(msg_win, COLOR_PAIR(COLOR_OTHER_MSG));
    if (selected) wattron(msg_win, A_REVERSE);
    mvwprintw(msg_win, cur_msg_y, 6, "[%s]: %s%s", sender, prefix, clean_body);
    if (is_me) wattroff(msg_win, COLOR_PAIR(COLOR_MY_MSG));
    else wattroff(msg_win, COLOR_PAIR(COLOR_OTHER_MSG));
    if (selected) wattroff(msg_win, A_REVERSE);
}

static void load_chat_history(int chat_id) {
    if (active_chat_idx < 0) return;
    chat_entry_t *chat = &chat_list[active_chat_idx];

    char **senders, **bodies;
    int *msg_ids, *reply_tos, *fwd_froms;
    int count = local_db_get_messages(chat_id, &senders, &bodies, &msg_ids, &reply_tos, &fwd_froms, 200);
    chat->msg_count = count;
    free(chat->server_msg_ids);
    chat->server_msg_ids = NULL;
    if (count > 0) {
        chat->server_msg_ids = malloc(count * sizeof(int));
        memcpy(chat->server_msg_ids, msg_ids, count * sizeof(int));
    }
    if (chat->selected_msg_idx >= count) chat->selected_msg_idx = count - 1;

    werase(msg_win);
    box(msg_win, 0, 0);
    mvwprintw(msg_win, 0, 1, "Чат: %s", chat->name);
    cur_msg_y = 1;
    for (int i = 0; i < count; i++) {
        int is_me = (strcmp(senders[i], my_login) == 0);
        int selected = (i == chat->selected_msg_idx);

        int reply_local = (reply_tos[i] >= 0) ? server_id_to_local(chat, reply_tos[i]) : -1;
        int fwd_server = fwd_froms[i];

        const char *reply_sender = NULL;
        const char *fwd_sender = NULL;

        if (reply_tos[i] >= 0) {
            for (int j = 0; j < count; j++) {
                if (msg_ids[j] == reply_tos[i]) {
                    reply_sender = senders[j];
                    break;
                }
            }
        }

        if (fwd_server >= 0) {
            for (int j = 0; j < count; j++) {
                if (msg_ids[j] == fwd_server) {
                    fwd_sender = senders[j];
                    break;
                }
            }
            if (!fwd_sender) {
                static char found_sender[MAX_LOGIN];
                if (find_sender_by_server_id(fwd_server, found_sender, sizeof(found_sender))) {
                    fwd_sender = found_sender;
                }
            }
        }

        print_msg_line(senders[i], bodies[i], i+1, is_me, selected,
                       reply_local, fwd_server, reply_sender, fwd_sender);
    }
    local_db_free_messages(count, senders, bodies, msg_ids, reply_tos, fwd_froms);
    wrefresh(msg_win);
}

void process_input(void) {
    if (active_chat_idx >= 0) {
        load_chat_history(chat_list[active_chat_idx].chat_id);
    }

    while (connected) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sockfd, &rfds);
        FD_SET(STDIN_FILENO, &rfds);
        struct timeval tv = {0, 50000};
        int maxfd = (sockfd > STDIN_FILENO) ? sockfd : STDIN_FILENO;

        int ready = select(maxfd+1, &rfds, NULL, NULL, &tv);
        if (ready < 0) break;

        pthread_mutex_lock(&in_queue.mutex);
        while (in_queue.count > 0) {
            char *msg = in_queue.messages[0].text;
            memmove(in_queue.messages, in_queue.messages+1,
                    (in_queue.count-1) * sizeof(msg_node_t));
            in_queue.count--;

            char cmd[MAX_CMD_LEN];
            get_cmd(msg, cmd, sizeof(cmd));

            if (strcmp(cmd, CMD_MSG) == 0 || strcmp(cmd, CMD_GRP) == 0 ||
                strcmp(cmd, CMD_REPLY) == 0 || strcmp(cmd, CMD_FWD) == 0) {
                char from[MAX_LOGIN], body[MAX_BODY];
                int chat_id, msg_id = -1;
                int reply_to = -1, fwd_from = -1;
                if (get_param(msg, "from", from, sizeof(from)) == 0 &&
                    get_param_int(msg, "chat_id", &chat_id) == 0 &&
                    get_param(msg, "text", body, sizeof(body)) == 0) {
                    get_param_int(msg, "msg_id", &msg_id);
                    get_param_int(msg, "reply_to", &reply_to);
                    get_param_int(msg, "fwd_from", &fwd_from);

                    if (strcmp(from, my_login) == 0) {
                        local_db_confirm_last_msg(chat_id, msg_id);
                    } else {
                        local_db_save_msg(chat_id, from, msg_id, body, reply_to, fwd_from);
                    }

                    int chat_idx = -1;
                    for (int i = 0; i < chat_count; i++) {
                        if (chat_list[i].chat_id == chat_id) { chat_idx = i; break; }
                    }
                    if (chat_idx == -1) {
                        add_chat(chat_id, from, 0);
                        chat_idx = chat_count - 1;
                        redraw_all();
                    }

                    if (active_chat_idx == chat_idx) {
                        chat_list[chat_idx].msg_count++;
                        load_chat_history(chat_id);
                    }
                }
            } else if (strcmp(cmd, CMD_OK) == 0) {
                int msg_id = -1, chat_id = -1;
                get_param_int(msg, "msg_id", &msg_id);
                get_param_int(msg, "chat_id", &chat_id);
                if (msg_id >= 0 && active_chat_idx >= 0) {
                    local_db_confirm_last_msg(chat_list[active_chat_idx].chat_id, msg_id);
                    load_chat_history(chat_list[active_chat_idx].chat_id);
                }
                char action[32] = {0};
                if (get_param(msg, "action", action, sizeof(action)) == 0) {
                    if (strcmp(action, "list") == 0) {
                        char users[MAX_MSG_LINE];
                        int count;
                        if (get_param(msg, "users", users, sizeof(users)) == 0 &&
                            get_param_int(msg, "count", &count) == 0) {
                            if (active_chat_idx >= 0) {
                                chat_entry_t *chat = &chat_list[active_chat_idx];
                                char buf[256];
                                snprintf(buf, sizeof(buf), "=== Онлайн (%d): %s ===", count, users);
                                local_db_save_msg(chat->chat_id, "система", -1, buf, -1, -1);
                                load_chat_history(chat->chat_id);
                            }
                        }
                    } else if (strcmp(action, "group_created") == 0) {
                        int gid;
                        char gname[MAX_LOGIN];
                        int member_count = 0;
                        if (get_param_int(msg, "chat_id", &gid) == 0 &&
                            get_param(msg, "name", gname, sizeof(gname)) == 0) {
                            get_param_int(msg, "member_count", &member_count);
                            add_chat(gid, gname, 1);
                            for (int i = 0; i < chat_count; i++) {
                                if (chat_list[i].chat_id == gid && chat_list[i].is_group) {
                                    chat_list[i].member_count = member_count;
                                    break;
                                }
                            }
                            send_cmd("GET_CHATS\n");
                            redraw_all();
                        }
                    }
                }
                if (chat_id >= 0 && !action[0]) {
                    send_cmd("GET_CHATS\n");
                }
            } else if (strcmp(cmd, CMD_ERR) == 0) {
                int code;
                char desc[MAX_MSG_LINE];
                if (get_param_int(msg, "code", &code) == 0 &&
                    get_param(msg, "desc", desc, sizeof(desc)) == 0) {
                    if (active_chat_idx >= 0) {
                        chat_entry_t *chat = &chat_list[active_chat_idx];
                        char buf[256];
                        snprintf(buf, sizeof(buf), "Ошибка %d: %s", code, desc);
                        local_db_save_msg(chat->chat_id, "система", -1, buf, -1, -1);
                        load_chat_history(chat->chat_id);
                    }
                }
            } else if (strcmp(cmd, "CHAT") == 0) {
                int chat_id = -1;
                char name[MAX_LOGIN] = {0};
                int is_group = 0;
                int member_count = 0;
                if (get_param_int(msg, "chat_id", &chat_id) == 0 &&
                    get_param(msg, "name", name, sizeof(name)) == 0) {
                    get_param_int(msg, "is_group", &is_group);
                    get_param_int(msg, "member_count", &member_count);
                    add_chat(chat_id, name, is_group);
                    for (int i = 0; i < chat_count; i++) {
                        if (chat_list[i].chat_id == chat_id && chat_list[i].is_group) {
                            chat_list[i].member_count = member_count;
                            break;
                        }
                    }
                    char req[MAX_MSG_LINE];
                    build_msg(req, sizeof(req), CMD_HIST, "|chat_id=%d", chat_id);
                    send_cmd(req);
                }
                redraw_all();
            } else {
                char *saveptr;
                char *tok = strtok_r(msg, "|", &saveptr);
                if (tok) {
                    int chat_id = atoi(tok);
                    char *sender = strtok_r(NULL, "|", &saveptr);
                    char *msg_id_str = strtok_r(NULL, "|", &saveptr);
                    char *body = strtok_r(NULL, "", &saveptr);
                    if (sender && msg_id_str && body) {
                        int msg_id = atoi(msg_id_str);
                        local_db_save_msg(chat_id, sender, msg_id, body, -1, -1);
                        int chat_idx = -1;
                        for (int i = 0; i < chat_count; i++) {
                            if (chat_list[i].chat_id == chat_id) { chat_idx = i; break; }
                        }
                        if (chat_idx == -1) {
                            add_chat(chat_id, sender, 0);
                            chat_idx = chat_count - 1;
                            redraw_all();
                        }
                        if (active_chat_idx == chat_idx) {
                            load_chat_history(chat_id);
                        }
                    }
                }
            }
        }
        pthread_mutex_unlock(&in_queue.mutex);

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            wint_t wch;
            while (1) {
                int ret = wget_wch(input_win, &wch);
                if (ret == ERR) break;
                if (ret == KEY_CODE_YES) {
                    if (wch == KEY_F(1)) { connected = 0; break; }
                    else if (wch == KEY_UP) {
                        if (input_mode == MODE_NORMAL) update_selection(-1);
                    }
                    else if (wch == KEY_DOWN) {
                        if (input_mode == MODE_NORMAL) update_selection(1);
                    }
                    else if (wch == KEY_BACKSPACE) {
                        if (input_len > 0) input_buf[--input_len] = L'\0';
                        display_input_line();
                    }
                    continue;
                }

                if (wch == L'\n') {
                    if (input_mode == MODE_FWD_SELECT_CHAT) {
                        strncpy(pending_fwd_user, chat_list[fwd_candidate_idx].name, MAX_LOGIN-1);
                        input_mode = MODE_FWD_TEXT;
                        input_len = 0;
                        memset(input_buf, 0, sizeof(input_buf));
                        display_input_line();
                        redraw_all();
                        continue;
                    }

                    if (input_len > 0 || input_mode == MODE_REPLY || input_mode == MODE_FWD_TEXT) {
                        char mb_text[MAX_BODY] = {0};
                        for (int i = 0; i < input_len; i++) {
                            char tmp[8] = {0};
                            int len = wctomb(tmp, input_buf[i]);
                            if (len > 0) strncat(mb_text, tmp, sizeof(mb_text)-strlen(mb_text)-1);
                        }

                        if (input_mode == MODE_REPLY) {
                            char clean_text[MAX_BODY];
                            sanitize_body(clean_text, mb_text, sizeof(clean_text));
                            if (active_chat_idx >= 0 && chat_list[active_chat_idx].is_group) {
                                char sendline[MAX_MSG_LINE];
                                build_msg(sendline, sizeof(sendline), CMD_GRP,
                                          "|chat_id=%d|text=%s|reply_to=%d",
                                          chat_list[active_chat_idx].chat_id, clean_text, pending_reply_srv_id);
                                send_cmd(sendline);
                                local_db_save_msg(chat_list[active_chat_idx].chat_id, my_login, -1,
                                                  clean_text, pending_reply_srv_id, -1);
                            } else {
                                char sendline[MAX_MSG_LINE];
                                build_msg(sendline, sizeof(sendline), CMD_REPLY,
                                          "|to=%s|text=%s|reply_to=%d",
                                          active_chat_idx >= 0 ? chat_list[active_chat_idx].name : "",
                                          clean_text, pending_reply_srv_id);
                                send_cmd(sendline);
                                local_db_save_msg(chat_list[active_chat_idx].chat_id, my_login, -1,
                                                  clean_text, pending_reply_srv_id, -1);
                            }
                            load_chat_history(chat_list[active_chat_idx].chat_id);
                            input_mode = MODE_NORMAL;
                            pending_reply_srv_id = pending_reply_local = 0;
                        } else if (input_mode == MODE_FWD_TEXT) {
                            char clean_text[MAX_BODY];
                            sanitize_body(clean_text, mb_text, sizeof(clean_text));
                            int dest_idx = -1;
                            for (int i = 0; i < chat_count; i++) {
                                if (strcmp(chat_list[i].name, pending_fwd_user) == 0) {
                                    dest_idx = i; break;
                                }
                            }
                            if (dest_idx >= 0) {
                                char final_text[MAX_BODY * 2];
                                snprintf(final_text, sizeof(final_text), "[from %s] %s",
                                         pending_fwd_original_sender, clean_text);
                                if (chat_list[dest_idx].is_group) {
                                    char sendline[MAX_MSG_LINE];
                                    build_msg(sendline, sizeof(sendline), CMD_GRP,
                                              "|chat_id=%d|text=%s|fwd_from=%d",
                                              chat_list[dest_idx].chat_id, final_text, pending_fwd_srv_id);
                                    send_cmd(sendline);
                                    local_db_save_msg(chat_list[dest_idx].chat_id, my_login, -1,
                                                      final_text, -1, pending_fwd_srv_id);
                                } else {
                                    char sendline[MAX_MSG_LINE];
                                    build_msg(sendline, sizeof(sendline), CMD_FWD,
                                              "|to=%s|text=%s|fwd_from=%d",
                                              pending_fwd_user, final_text, pending_fwd_srv_id);
                                    send_cmd(sendline);
                                    local_db_save_msg(chat_list[dest_idx].chat_id, my_login, -1,
                                                      final_text, -1, pending_fwd_srv_id);
                                }
                                active_chat_idx = dest_idx;
                                load_chat_history(chat_list[active_chat_idx].chat_id);
                            }
                            input_mode = MODE_NORMAL;
                            pending_fwd_srv_id = pending_fwd_local = 0;
                            pending_fwd_user[0] = '\0';
                            pending_fwd_original_sender[0] = '\0';
                        } else if (mb_text[0] == '/') {
                            process_command(mb_text);
                        } else if (active_chat_idx >= 0) {
                            chat_entry_t *chat = &chat_list[active_chat_idx];
                            char clean_text[MAX_BODY];
                            sanitize_body(clean_text, mb_text, sizeof(clean_text));
                            char sendline[MAX_MSG_LINE];
                            if (chat->is_group)
                                build_msg(sendline, sizeof(sendline), CMD_GRP,
                                          "|chat_id=%d|text=%s", chat->chat_id, clean_text);
                            else
                                build_msg(sendline, sizeof(sendline), CMD_MSG,
                                          "|to=%s|text=%s", chat->name, clean_text);
                            send_cmd(sendline);
                            local_db_save_msg(chat->chat_id, my_login, -1, clean_text, -1, -1);
                            load_chat_history(chat->chat_id);
                        }
                        input_len = 0;
                        memset(input_buf, 0, sizeof(input_buf));
                    }
                    display_input_line();
                    redraw_all();
                } else if (wch == 18) {  // Ctrl+R
                    if (active_chat_idx >= 0 && chat_list[active_chat_idx].selected_msg_idx >= 0) {
                        chat_entry_t *chat = &chat_list[active_chat_idx];
                        pending_reply_srv_id = chat->server_msg_ids[chat->selected_msg_idx];
                        pending_reply_local = chat->selected_msg_idx + 1;
                        input_mode = MODE_REPLY;
                        input_len = 0;
                        memset(input_buf, 0, sizeof(input_buf));
                        display_input_line();
                        redraw_all();
                    }
                } else if (wch == 6) {  // Ctrl+F
                    if (active_chat_idx >= 0 && chat_list[active_chat_idx].selected_msg_idx >= 0) {
                        chat_entry_t *chat = &chat_list[active_chat_idx];
                        pending_fwd_srv_id = chat->server_msg_ids[chat->selected_msg_idx];
                        pending_fwd_local = chat->selected_msg_idx + 1;
                        int srv_id = pending_fwd_srv_id;
                        char orig_sender[MAX_LOGIN] = "unknown";
                        local_db_get_msg_sender(chat->chat_id, srv_id, orig_sender, sizeof(orig_sender));
                        strncpy(pending_fwd_original_sender, orig_sender, MAX_LOGIN-1);
                        input_mode = MODE_FWD_SELECT_CHAT;
                        fwd_candidate_idx = (active_chat_idx + 1) % chat_count;
                        input_len = 0;
                        memset(input_buf, 0, sizeof(input_buf));
                        display_input_line();
                        redraw_all();
                    }
                } else if (wch == L'\t') {
                    if (input_mode == MODE_FWD_SELECT_CHAT) {
                        if (chat_count > 1) {
                            fwd_candidate_idx = (fwd_candidate_idx + 1) % chat_count;
                            display_input_line();
                            redraw_all();
                        }
                    } else if (input_mode == MODE_NORMAL) {
                        if (chat_count > 0) {
                            input_mode = MODE_NORMAL;
                            input_len = 0; memset(input_buf, 0, sizeof(input_buf));
                            active_chat_idx = (active_chat_idx + 1) % chat_count;
                            load_chat_history(chat_list[active_chat_idx].chat_id);
                            redraw_all();
                        }
                    }
                } else if (wch >= 32 && input_len < MAX_BODY-1) {
                    input_buf[input_len++] = wch;
                    input_buf[input_len] = L'\0';
                    display_input_line();
                }
            }
        }
    }
}