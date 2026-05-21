#include "client.h"
#include "local_db.h"
#include <ncurses.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/select.h>

chat_entry_t chat_list[MAX_CHATS];
int chat_count = 0;
int active_chat_idx = -1;

static WINDOW *chat_win, *msg_win, *input_win, *help_win;
static int msg_win_h, msg_win_w;
static int chat_win_w;
static int show_help = 0;

static char input_buf[MAX_BODY];
static int input_len = 0;

static int cur_msg_y = 0;

enum { COLOR_MY_MSG = 1, COLOR_OTHER_MSG, COLOR_HIGHLIGHT, COLOR_GROUP, COLOR_INFO };

static void load_chat_history(int chat_id);

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
    mvwprintw(help_win, 0, 0, "Commands: /msg <user> <text> | /list | /help | /quit");
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
    werase(msg_win);
    box(msg_win, 0, 0);
    werase(input_win);
    werase(help_win);

    mvwprintw(chat_win, 0, 1, "Chats (%d)", chat_count);
    for (int i = 0; i < chat_count; i++) {
        int y = i + 1;
        if (y >= msg_win_h) break;
        if (i == active_chat_idx)
            wattron(chat_win, COLOR_PAIR(COLOR_HIGHLIGHT));
        else if (chat_list[i].is_group)
            wattron(chat_win, COLOR_PAIR(COLOR_GROUP));
        mvwprintw(chat_win, y, 1, "%s", chat_list[i].name);
        if (i == active_chat_idx)
            wattroff(chat_win, COLOR_PAIR(COLOR_HIGHLIGHT));
        else if (chat_list[i].is_group)
            wattroff(chat_win, COLOR_PAIR(COLOR_GROUP));
    }

    if (active_chat_idx >= 0) {
        mvwprintw(msg_win, 0, 1, "Chat: %s", chat_list[active_chat_idx].name);
    }

    mvwprintw(input_win, 0, 0, "> %s", input_buf);
    wmove(input_win, 0, 2 + input_len);

    wattron(help_win, COLOR_PAIR(COLOR_INFO));
    if (show_help) {
        mvwprintw(help_win, 0, 0, "Commands: /msg /group /create /list /help /quit | TAB/Arrows: navigate");
    } else {
        mvwprintw(help_win, 0, 0, "Type /help for commands | F1: quit | TAB: switch chat");
    }
    wattroff(help_win, COLOR_PAIR(COLOR_INFO));

    wrefresh(chat_win);
    wrefresh(msg_win);
    wrefresh(input_win);
    wrefresh(help_win);
}

void add_chat(int id, const char *name, int is_group) {
    for (int i = 0; i < chat_count; i++) {
        if (strcmp(chat_list[i].name, name) == 0) {
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
        chat_count++;
        if (active_chat_idx < 0) active_chat_idx = 0;
        local_db_add_chat(id, name, is_group);
    }
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
        char *text = strtok_r(NULL, "", &saveptr);
        if (user && text) {
            if (strcmp(user, my_login) == 0) {
                if (active_chat_idx >= 0) {
                    if (cur_msg_y >= msg_win_h - 1) {
                        scroll(msg_win);
                        cur_msg_y = msg_win_h - 2;
                    }
                    wattron(msg_win, COLOR_PAIR(COLOR_INFO));
                    mvwprintw(msg_win, ++cur_msg_y, 1, "Cannot message yourself.");
                    wattroff(msg_win, COLOR_PAIR(COLOR_INFO));
                    wrefresh(msg_win);
                }
                return;
            }

            int existing_id = -1;
            for (int i = 0; i < chat_count; i++) {
                if (strcmp(chat_list[i].name, user) == 0) {
                    existing_id = chat_list[i].chat_id;
                    break;
                }
            }
            add_chat(existing_id, user, 0);
            char sendline[MAX_MSG_LINE];
            build_msg(sendline, sizeof(sendline), CMD_MSG,
                      "|to=%s|text=%s", user, text);
            send_cmd(sendline);
            local_db_save_msg(existing_id, my_login, -1, text);

            if (active_chat_idx >= 0 && strcmp(chat_list[active_chat_idx].name, user) == 0) {
                if (cur_msg_y >= msg_win_h - 1) {
                    scroll(msg_win);
                    cur_msg_y = msg_win_h - 2;
                }
                wattron(msg_win, COLOR_PAIR(COLOR_MY_MSG));
                mvwprintw(msg_win, ++cur_msg_y, 1, "[%s]: %s", my_login, text);
                wattroff(msg_win, COLOR_PAIR(COLOR_MY_MSG));
                wrefresh(msg_win);
            }
        }
    } else if (strcmp(cmd, "group") == 0) {
        char *id_str = strtok_r(NULL, " ", &saveptr);
        char *text = strtok_r(NULL, "", &saveptr);
        if (id_str && text) {
            int gid = atoi(id_str);
            char sendline[MAX_MSG_LINE];
            build_msg(sendline, sizeof(sendline), CMD_GRP,
                      "|chat_id=%d|text=%s", gid, text);
            send_cmd(sendline);
            local_db_save_msg(gid, my_login, -1, text);
            if (active_chat_idx >= 0 && chat_list[active_chat_idx].chat_id == gid) {
                if (cur_msg_y >= msg_win_h - 1) {
                    scroll(msg_win);
                    cur_msg_y = msg_win_h - 2;
                }
                wattron(msg_win, COLOR_PAIR(COLOR_MY_MSG));
                mvwprintw(msg_win, ++cur_msg_y, 1, "[%s]: %s", my_login, text);
                wattroff(msg_win, COLOR_PAIR(COLOR_MY_MSG));
                wrefresh(msg_win);
            }
        }
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
        werase(help_win);
        wattron(help_win, COLOR_PAIR(COLOR_INFO));
        if (show_help) {
            mvwprintw(help_win, 0, 0, "Commands: /msg /group /create /list /help /quit | TAB/Arrows: navigate");
        } else {
            mvwprintw(help_win, 0, 0, "Type /help for commands | F1: quit | TAB: switch chat");
        }
        wattroff(help_win, COLOR_PAIR(COLOR_INFO));
        wrefresh(help_win);
    } else if (strcmp(cmd, "quit") == 0) {
        connected = 0;
    }
}

static void load_chat_history(int chat_id) {
    if (chat_id < 0) return;
    char **senders, **bodies;
    int count = local_db_get_messages(chat_id, &senders, &bodies, 200);
    werase(msg_win);
    box(msg_win, 0, 0);
    if (active_chat_idx >= 0) {
        mvwprintw(msg_win, 0, 1, "Chat: %s", chat_list[active_chat_idx].name);
    }
    cur_msg_y = 1;
    for (int i = 0; i < count; i++) {
        if (cur_msg_y >= msg_win_h - 1) break;
        if (strcmp(senders[i], my_login) == 0) {
            wattron(msg_win, COLOR_PAIR(COLOR_MY_MSG));
        } else {
            wattron(msg_win, COLOR_PAIR(COLOR_OTHER_MSG));
        }
        mvwprintw(msg_win, cur_msg_y, 1, "[%s]: %s", senders[i], bodies[i]);
        if (strcmp(senders[i], my_login) == 0) {
            wattroff(msg_win, COLOR_PAIR(COLOR_MY_MSG));
        } else {
            wattroff(msg_win, COLOR_PAIR(COLOR_OTHER_MSG));
        }
        cur_msg_y++;
    }
    local_db_free_messages(count, senders, bodies);
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
                if (get_param(msg, "from", from, sizeof(from)) == 0 &&
                    get_param_int(msg, "chat_id", &chat_id) == 0 &&
                    get_param(msg, "text", body, sizeof(body)) == 0) {
                    get_param_int(msg, "msg_id", &msg_id);

                    if (strcmp(from, my_login) == 0) {
                        local_db_confirm_last_msg(chat_id, msg_id);
                    } else {
                        local_db_save_msg(chat_id, from, msg_id, body);
                    }

                    int found = 0;
                    for (int i = 0; i < chat_count; i++) {
                        if (chat_list[i].chat_id == chat_id) { found = 1; break; }
                    }
                    if (!found) {
                        add_chat(chat_id, from, 0);
                        werase(chat_win);
                        box(chat_win, 0, 0);
                        mvwprintw(chat_win, 0, 1, "Chats (%d)", chat_count);
                        for (int i = 0; i < chat_count; i++) {
                            int y = i + 1;
                            if (y >= msg_win_h) break;
                            if (i == active_chat_idx)
                                wattron(chat_win, COLOR_PAIR(COLOR_HIGHLIGHT));
                            else if (chat_list[i].is_group)
                                wattron(chat_win, COLOR_PAIR(COLOR_GROUP));
                            mvwprintw(chat_win, y, 1, "%s", chat_list[i].name);
                            if (i == active_chat_idx)
                                wattroff(chat_win, COLOR_PAIR(COLOR_HIGHLIGHT));
                            else if (chat_list[i].is_group)
                                wattroff(chat_win, COLOR_PAIR(COLOR_GROUP));
                        }
                        wrefresh(chat_win);
                    }

                    if (active_chat_idx >= 0 && chat_list[active_chat_idx].chat_id == chat_id) {
                        if (cur_msg_y >= msg_win_h - 1) {
                            scroll(msg_win);
                            cur_msg_y = msg_win_h - 2;
                        }
                        if (strcmp(from, my_login) == 0) {
                            wattron(msg_win, COLOR_PAIR(COLOR_MY_MSG));
                        } else {
                            wattron(msg_win, COLOR_PAIR(COLOR_OTHER_MSG));
                        }
                        mvwprintw(msg_win, ++cur_msg_y, 1, "[%s]: %s", from, body);
                        if (strcmp(from, my_login) == 0) {
                            wattroff(msg_win, COLOR_PAIR(COLOR_MY_MSG));
                        } else {
                            wattroff(msg_win, COLOR_PAIR(COLOR_OTHER_MSG));
                        }
                        wrefresh(msg_win);
                    }
                }
            } else if (strcmp(cmd, CMD_OK) == 0) {
                int msg_id = -1;
                if (get_param_int(msg, "msg_id", &msg_id) == 0 && active_chat_idx >= 0) {
                    local_db_confirm_last_msg(chat_list[active_chat_idx].chat_id, msg_id);
                }
                char action[32] = {0};
                if (get_param(msg, "action", action, sizeof(action)) == 0) {
                    if (strcmp(action, "list") == 0) {
                        char users[MAX_MSG_LINE];
                        int count;
                        if (get_param(msg, "users", users, sizeof(users)) == 0 &&
                            get_param_int(msg, "count", &count) == 0) {
                            if (active_chat_idx >= 0) {
                                if (cur_msg_y >= msg_win_h - 1) {
                                    scroll(msg_win);
                                    cur_msg_y = msg_win_h - 2;
                                }
                                wattron(msg_win, COLOR_PAIR(COLOR_INFO));
                                mvwprintw(msg_win, ++cur_msg_y, 1, "=== Online (%d): %s ===", count, users);
                                wattroff(msg_win, COLOR_PAIR(COLOR_INFO));
                                wrefresh(msg_win);
                            }
                        }
                    } else if (strcmp(action, "group_created") == 0) {
                        int gid;
                        char gname[MAX_LOGIN];
                        if (get_param_int(msg, "chat_id", &gid) == 0 &&
                            get_param(msg, "name", gname, sizeof(gname)) == 0) {
                            add_chat(gid, gname, 1);
                            werase(chat_win);
                            box(chat_win, 0, 0);
                            mvwprintw(chat_win, 0, 1, "Chats (%d)", chat_count);
                            for (int i = 0; i < chat_count; i++) {
                                int y = i + 1;
                                if (y >= msg_win_h) break;
                                if (i == active_chat_idx)
                                    wattron(chat_win, COLOR_PAIR(COLOR_HIGHLIGHT));
                                else if (chat_list[i].is_group)
                                    wattron(chat_win, COLOR_PAIR(COLOR_GROUP));
                                mvwprintw(chat_win, y, 1, "%s", chat_list[i].name);
                                if (i == active_chat_idx)
                                    wattroff(chat_win, COLOR_PAIR(COLOR_HIGHLIGHT));
                                else if (chat_list[i].is_group)
                                    wattroff(chat_win, COLOR_PAIR(COLOR_GROUP));
                            }
                            wrefresh(chat_win);
                            if (active_chat_idx >= 0 && chat_list[active_chat_idx].chat_id == gid) {
                                load_chat_history(gid);
                            }
                        }
                    } else if (strcmp(action, "help") == 0) {
                        char text[MAX_MSG_LINE];
                        if (get_param(msg, "text", text, sizeof(text)) == 0) {
                            if (active_chat_idx >= 0) {
                                if (cur_msg_y >= msg_win_h - 1) {
                                    scroll(msg_win);
                                    cur_msg_y = msg_win_h - 2;
                                }
                                wattron(msg_win, COLOR_PAIR(COLOR_INFO));
                                mvwprintw(msg_win, ++cur_msg_y, 1, "%s", text);
                                wattroff(msg_win, COLOR_PAIR(COLOR_INFO));
                                wrefresh(msg_win);
                            }
                        }
                    }
                }
            } else if (strcmp(cmd, CMD_ERR) == 0) {
                int code;
                char desc[MAX_MSG_LINE];
                if (get_param_int(msg, "code", &code) == 0 &&
                    get_param(msg, "desc", desc, sizeof(desc)) == 0) {
                    if (active_chat_idx >= 0) {
                        if (cur_msg_y >= msg_win_h - 1) {
                            scroll(msg_win);
                            cur_msg_y = msg_win_h - 2;
                        }
                        wattron(msg_win, COLOR_PAIR(COLOR_INFO));
                        mvwprintw(msg_win, ++cur_msg_y, 1, "Error %d: %s", code, desc);
                        wattroff(msg_win, COLOR_PAIR(COLOR_INFO));
                        wrefresh(msg_win);
                    }
                }
            } else if (strcmp(cmd, "CHAT") == 0) {
                int chat_id = -1;
                char name[MAX_LOGIN] = {0};
                int is_group = 0;
                if (get_param_int(msg, "chat_id", &chat_id) == 0 &&
                    get_param(msg, "name", name, sizeof(name)) == 0) {
                    get_param_int(msg, "is_group", &is_group);
                    // Имя теперь всегда приходит правильное с сервера
                    add_chat(chat_id, name, is_group);
                    // Запрашиваем историю этого чата
                    char req[MAX_MSG_LINE];
                    build_msg(req, sizeof(req), CMD_HIST, "|chat_id=%d", chat_id);
                    send_cmd(req);
                }
                // Обновляем панель чатов
                werase(chat_win);
                box(chat_win, 0, 0);
                mvwprintw(chat_win, 0, 1, "Chats (%d)", chat_count);
                for (int i = 0; i < chat_count; i++) {
                    int y = i + 1;
                    if (y >= msg_win_h) break;
                    if (i == active_chat_idx)
                        wattron(chat_win, COLOR_PAIR(COLOR_HIGHLIGHT));
                    else if (chat_list[i].is_group)
                        wattron(chat_win, COLOR_PAIR(COLOR_GROUP));
                    mvwprintw(chat_win, y, 1, "%s", chat_list[i].name);
                    if (i == active_chat_idx)
                        wattroff(chat_win, COLOR_PAIR(COLOR_HIGHLIGHT));
                    else if (chat_list[i].is_group)
                        wattroff(chat_win, COLOR_PAIR(COLOR_GROUP));
                }
                wrefresh(chat_win);
            } else {
                // Строка истории с сервера: chat_id|sender|msg_id|body
                char *saveptr;
                char *tok = strtok_r(msg, "|", &saveptr);
                if (tok) {
                    int chat_id = atoi(tok);
                    char *sender = strtok_r(NULL, "|", &saveptr);
                    char *msg_id_str = strtok_r(NULL, "|", &saveptr);
                    char *body = strtok_r(NULL, "", &saveptr);
                    if (sender && msg_id_str && body) {
                        int msg_id = atoi(msg_id_str);
                        local_db_save_msg(chat_id, sender, msg_id, body);

                        int found = 0;
                        for (int i = 0; i < chat_count; i++) {
                            if (chat_list[i].chat_id == chat_id) { found = 1; break; }
                        }
                        if (!found) {
                            add_chat(chat_id, sender, 0);
                            werase(chat_win);
                            box(chat_win, 0, 0);
                            mvwprintw(chat_win, 0, 1, "Chats (%d)", chat_count);
                            for (int i = 0; i < chat_count; i++) {
                                int y = i + 1;
                                if (y >= msg_win_h) break;
                                if (i == active_chat_idx)
                                    wattron(chat_win, COLOR_PAIR(COLOR_HIGHLIGHT));
                                else if (chat_list[i].is_group)
                                    wattron(chat_win, COLOR_PAIR(COLOR_GROUP));
                                mvwprintw(chat_win, y, 1, "%s", chat_list[i].name);
                                if (i == active_chat_idx)
                                    wattroff(chat_win, COLOR_PAIR(COLOR_HIGHLIGHT));
                                else if (chat_list[i].is_group)
                                    wattroff(chat_win, COLOR_PAIR(COLOR_GROUP));
                            }
                            wrefresh(chat_win);
                        }

                        // Перерисовка истории активного чата
                        if (active_chat_idx >= 0 && chat_list[active_chat_idx].chat_id == chat_id) {
                            load_chat_history(chat_id);
                        }
                    }
                }
            }
        }
        pthread_mutex_unlock(&in_queue.mutex);

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            int ch;
            while ((ch = wgetch(input_win)) != ERR) {
                if (ch == KEY_F(1)) { connected = 0; break; }
                else if (ch == '\n') {
                    if (input_len > 0) {
                        input_buf[input_len] = '\0';
                        if (input_buf[0] == '/') {
                            process_command(input_buf);
                        } else if (active_chat_idx >= 0) {
                            chat_entry_t *chat = &chat_list[active_chat_idx];
                            char sendline[MAX_MSG_LINE];
                            if (chat->is_group)
                                build_msg(sendline, sizeof(sendline), CMD_GRP,
                                          "|chat_id=%d|text=%s", chat->chat_id, input_buf);
                            else
                                build_msg(sendline, sizeof(sendline), CMD_MSG,
                                          "|to=%s|text=%s", chat->name, input_buf);
                            send_cmd(sendline);
                            local_db_save_msg(chat->chat_id, my_login, -1, input_buf);

                            if (cur_msg_y >= msg_win_h - 1) {
                                scroll(msg_win);
                                cur_msg_y = msg_win_h - 2;
                            }
                            wattron(msg_win, COLOR_PAIR(COLOR_MY_MSG));
                            mvwprintw(msg_win, ++cur_msg_y, 1, "[%s]: %s", my_login, input_buf);
                            wattroff(msg_win, COLOR_PAIR(COLOR_MY_MSG));
                            wrefresh(msg_win);
                        }
                        input_len = 0;
                        memset(input_buf, 0, sizeof(input_buf));
                    }
                    werase(input_win);
                    mvwprintw(input_win, 0, 0, "> %s", input_buf);
                    wmove(input_win, 0, 2 + input_len);
                    wrefresh(input_win);
                } else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b') {
                    if (input_len > 0) input_buf[--input_len] = '\0';
                    werase(input_win);
                    mvwprintw(input_win, 0, 0, "> %s", input_buf);
                    wmove(input_win, 0, 2 + input_len);
                    wrefresh(input_win);
                } else if (ch >= 32 && ch < 127 && input_len < (int)sizeof(input_buf)-1) {
                    input_buf[input_len++] = (char)ch;
                    input_buf[input_len] = '\0';
                    werase(input_win);
                    mvwprintw(input_win, 0, 0, "> %s", input_buf);
                    wmove(input_win, 0, 2 + input_len);
                    wrefresh(input_win);
                } else if (ch == '\t' || ch == KEY_UP || ch == KEY_DOWN) {
                    if (ch == '\t' && chat_count > 0)
                        active_chat_idx = (active_chat_idx + 1) % chat_count;
                    else if (ch == KEY_UP && active_chat_idx > 0)
                        active_chat_idx--;
                    else if (ch == KEY_DOWN && active_chat_idx < chat_count - 1)
                        active_chat_idx++;

                    if (active_chat_idx >= 0) {
                        load_chat_history(chat_list[active_chat_idx].chat_id);
                    }

                    werase(chat_win);
                    box(chat_win, 0, 0);
                    mvwprintw(chat_win, 0, 1, "Chats (%d)", chat_count);
                    for (int i = 0; i < chat_count; i++) {
                        int y = i + 1;
                        if (y >= msg_win_h) break;
                        if (i == active_chat_idx)
                            wattron(chat_win, COLOR_PAIR(COLOR_HIGHLIGHT));
                        else if (chat_list[i].is_group)
                            wattron(chat_win, COLOR_PAIR(COLOR_GROUP));
                        mvwprintw(chat_win, y, 1, "%s", chat_list[i].name);
                        if (i == active_chat_idx)
                            wattroff(chat_win, COLOR_PAIR(COLOR_HIGHLIGHT));
                        else if (chat_list[i].is_group)
                            wattroff(chat_win, COLOR_PAIR(COLOR_GROUP));
                    }
                    wrefresh(chat_win);
                }
            }
        }
    }
}