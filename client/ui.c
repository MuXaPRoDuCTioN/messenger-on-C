#define _GNU_SOURCE
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
static int max_name_width;
static int help_state = 0;          /* 0 = управление, 1 = команды */

static wchar_t input_buf[MAX_BODY];
static int input_len = 0;

/*
 * msg_scroll_offset — смещение (в строках) от ПОСЛЕДНЕЙ строки истории.
 * 0 = показываем самый конец (последние сообщения внизу).
 * Увеличивается при прокрутке вверх, уменьшается при прокрутке вниз.
 */
static int msg_scroll_offset = 0;

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
static char pending_fwd_original_body[MAX_BODY] = {0};
static int fwd_candidate_idx = 0;

enum { COLOR_MY_MSG = 1, COLOR_OTHER_MSG, COLOR_HIGHLIGHT, COLOR_GROUP,
       COLOR_INFO, COLOR_ID, COLOR_SELECTED };

/* ===== forward declarations ===== */
static size_t utf8_char_len(unsigned char c);
static int mb_str_width(const char *s);
static size_t bytes_for_cols(const char *str, int max_cols);
static void load_chat_history(int chat_id);
static void draw_msg_win(const char *chat_name,
                         char **senders, char **bodies,
                         int *msg_ids, int *reply_tos, int *fwd_froms,
                         int count, int selected_idx);
static void sanitize_body(char *dest, const char *src, size_t dest_size);
static void display_input_line(void);
static int server_id_to_local(const chat_entry_t *chat, int server_id);
static void truncate_name_for_panel(char *out, size_t out_size,
                                    const char *name, const char *suffix,
                                    int max_width);
void process_command(const char *cmd_line);

/* ========== INIT / CLEANUP ========== */

void init_ui(void) {
    initscr();
    cbreak();
    noecho();
    curs_set(1);
    start_color();
    init_pair(COLOR_MY_MSG,    COLOR_GREEN,   COLOR_BLACK);
    init_pair(COLOR_OTHER_MSG, COLOR_WHITE,   COLOR_BLACK);
    init_pair(COLOR_HIGHLIGHT, COLOR_CYAN,    COLOR_BLACK);
    init_pair(COLOR_GROUP,     COLOR_YELLOW,  COLOR_BLACK);
    init_pair(COLOR_INFO,      COLOR_MAGENTA, COLOR_BLACK);
    init_pair(COLOR_ID,        COLOR_BLACK,   COLOR_WHITE);
    init_pair(COLOR_SELECTED,  COLOR_BLACK,   COLOR_CYAN);

    int h = LINES - 2, w = COLS;
    chat_win_w = w / 4;
    if (chat_win_w < 12) chat_win_w = 12;
    chat_win  = newwin(h, chat_win_w,     0, 0);
    msg_win   = newwin(h, w - chat_win_w, 0, chat_win_w);
    input_win = newwin(1, w, LINES - 2,   0);
    help_win  = newwin(1, w, LINES - 1,   0);
    keypad(input_win, TRUE);
    nodelay(input_win, TRUE);
    /* scrollok выключен — мы управляем скроллингом вручную */
    msg_win_h = h;
    msg_win_w = w - chat_win_w;
    max_name_width = chat_win_w - 2;

    /* строка-подсказка */
    wattron(help_win, COLOR_PAIR(COLOR_INFO));
    mvwaddstr(help_win, 0, 0,
              "F1 выход | Tab чаты | ↑↓ выбор | PgUp/PgDn прокрутка | Ctrl+R ответ | Ctrl+F переслать");
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

/* ========== UTF-8 / WIDTH HELPERS ========== */

static size_t utf8_char_len(unsigned char c) {
    if (c < 0x80) return 1;
    if (c < 0xC0) return 0;       /* continuation byte */
    if (c < 0xE0) return 2;
    if (c < 0xF0) return 3;
    if (c < 0xF8) return 4;
    return 0;
}

static int mb_str_width(const char *s) {
    int width = 0;
    wchar_t wc;
    int n;
    while (*s) {
        n = mbtowc(&wc, s, MB_CUR_MAX);
        if (n <= 0) break;
        int w = wcwidth(wc);
        if (w > 0) width += w;
        s += n;
    }
    return width;
}

/*
 * bytes_for_cols — возвращает количество БАЙТ из строки str,
 * которые помещаются в max_cols экранных колонок (без обрывания символа).
 */
static size_t bytes_for_cols(const char *str, int max_cols) {
    const char *p = str;
    int cols = 0;
    while (*p) {
        wchar_t wc;
        int bytes = mbtowc(&wc, p, MB_CUR_MAX);
        if (bytes <= 0) { p++; continue; }
        int w = wcwidth(wc);
        if (w < 0) w = 1;
        if (cols + w > max_cols) break;
        cols += w;
        p += bytes;
    }
    return (size_t)(p - str);
}

/* обрезает имя чата для левой панели */
static void truncate_name_for_panel(char *out, size_t out_size,
                                    const char *name, const char *suffix,
                                    int max_width) {
    int suffix_w = mb_str_width(suffix);
    int name_w   = mb_str_width(name);
    if (name_w + suffix_w <= max_width) {
        snprintf(out, out_size, "%s%s", name, suffix);
    } else {
        int limit_cols = max_width - suffix_w;
        if (limit_cols < 0) limit_cols = 0;
        size_t cut = bytes_for_cols(name, limit_cols);
        snprintf(out, out_size, "%.*s%s", (int)cut, name, suffix);
    }
}

/* ========== REDRAW ========== */

void redraw_all(void) {
    werase(chat_win);
    box(chat_win, 0, 0);
    werase(input_win);
    werase(help_win);

    mvwprintw(chat_win, 0, 1, "Чаты (%d)", chat_count);
    for (int i = 0; i < chat_count; i++) {
        int y = i + 1;
        if (y >= msg_win_h) break;

        char displayed_name[MAX_LOGIN + 10];
        if (chat_list[i].is_group) {
            char suffix[16];
            snprintf(suffix, sizeof(suffix), "(%d)", chat_list[i].member_count);
            truncate_name_for_panel(displayed_name, sizeof(displayed_name),
                                    chat_list[i].name, suffix, max_name_width);
        } else {
            truncate_name_for_panel(displayed_name, sizeof(displayed_name),
                                    chat_list[i].name, "", max_name_width);
        }

        if (input_mode == MODE_FWD_SELECT_CHAT && i == fwd_candidate_idx)
            wattron(chat_win, COLOR_PAIR(COLOR_SELECTED));
        else if (i == active_chat_idx)
            wattron(chat_win, COLOR_PAIR(COLOR_HIGHLIGHT));
        else if (chat_list[i].is_group)
            wattron(chat_win, COLOR_PAIR(COLOR_GROUP));

        mvwaddstr(chat_win, y, 1, displayed_name);

        if (input_mode == MODE_FWD_SELECT_CHAT && i == fwd_candidate_idx)
            wattroff(chat_win, COLOR_PAIR(COLOR_SELECTED));
        else if (i == active_chat_idx)
            wattroff(chat_win, COLOR_PAIR(COLOR_HIGHLIGHT));
        else if (chat_list[i].is_group)
            wattroff(chat_win, COLOR_PAIR(COLOR_GROUP));
    }

    display_input_line();

    wattron(help_win, COLOR_PAIR(COLOR_INFO));
    if (help_state == 1) {
        mvwaddstr(help_win, 0, 0,
            "/help управление | Команды: /msg <user> создать чат | /create <name> <u1,u2> группа");
    } else {
        mvwaddstr(help_win, 0, 0,
            "F1 выход | Tab чаты | ↑↓ выбор | PgUp/PgDn прокрутка | Ctrl+R ответ | Ctrl+F переслать");
    }
    wattroff(help_win, COLOR_PAIR(COLOR_INFO));

    wrefresh(chat_win);
    wrefresh(input_win);
    wrefresh(help_win);
}

/* ========== INPUT LINE ========== */

static void display_input_line(void) {
    werase(input_win);
    char prefix_buf[80];
    const char *prefix = "> ";

    if (input_mode == MODE_REPLY) {
        snprintf(prefix_buf, sizeof(prefix_buf), ">> ответ на #%d: ", pending_reply_local);
        prefix = prefix_buf;
    } else if (input_mode == MODE_FWD_SELECT_CHAT) {
        snprintf(prefix_buf, sizeof(prefix_buf), ">> переслать #%d -> %s (Enter): ",
                 pending_fwd_local, chat_list[fwd_candidate_idx].name);
        prefix = prefix_buf;
    } else if (input_mode == MODE_FWD_TEXT) {
        snprintf(prefix_buf, sizeof(prefix_buf), ">> переслать #%d -> %s: ",
                 pending_fwd_local, pending_fwd_user);
        prefix = prefix_buf;
    }

    char mb_text[MAX_BODY * 4] = {0};
    for (int i = 0; i < input_len; i++) {
        char tmp[8] = {0};
        if (wctomb(tmp, input_buf[i]) > 0)
            strncat(mb_text, tmp, sizeof(mb_text) - strlen(mb_text) - 1);
    }

    int prefix_w    = mb_str_width(prefix);
    int max_text_cols = COLS - prefix_w - 1;
    if (max_text_cols < 10) max_text_cols = 10;

    /* показываем последние символы, если строка длиннее экрана */
    size_t text_bytes = strlen(mb_text);
    const char *show_from = mb_text;
    int text_cols = mb_str_width(mb_text);
    if (text_cols > max_text_cols) {
        const char *p = mb_text + text_bytes;
        int cols = 0;
        while (p > mb_text) {
            p--;
            while (p > mb_text && utf8_char_len((unsigned char)*p) == 0) p--;
            wchar_t wc;
            int n = mbtowc(&wc, p, MB_CUR_MAX);
            int w = (n > 0) ? wcwidth(wc) : 1;
            if (w < 0) w = 1;
            if (cols + w > max_text_cols) { p += (n > 0 ? n : 1); break; }
            cols += w;
        }
        show_from = p;
    }

    mvwaddstr(input_win, 0, 0, prefix);
    waddstr(input_win, show_from);
    wrefresh(input_win);
}

/* ========== CHAT LIST ========== */

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
        chat_list[chat_count].chat_id         = id;
        strncpy(chat_list[chat_count].name, name, MAX_LOGIN - 1);
        chat_list[chat_count].name[MAX_LOGIN - 1] = '\0';
        chat_list[chat_count].is_group        = is_group;
        chat_list[chat_count].msg_count       = 0;
        chat_list[chat_count].server_msg_ids  = NULL;
        chat_list[chat_count].selected_msg_idx = -1;
        chat_list[chat_count].member_count    = 0;
        chat_count++;
        if (active_chat_idx < 0) active_chat_idx = 0;
        local_db_add_chat(id, name, is_group);
    }
}

/* ========== BODY HELPERS ========== */

static void sanitize_body(char *dest, const char *src, size_t dest_size) {
    const char *p = src;
    char *q = dest;
    while (*p && (size_t)(q - dest) < dest_size - 1) {
        if      (*p == '|')  { *q++ = ';'; p++; }
        else if (*p == '\n') { *q++ = ' '; p++; }
        else                  *q++ = *p++;
    }
    *q = '\0';
}

/* ========== COMMANDS ========== */

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
        if (mb_str_width(user) > max_name_width) {
            werase(input_win);
            mvwaddstr(input_win, 0, 0, "Слишком длинное имя пользователя");
            wrefresh(input_win);
            return;
        }
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
        input_len = 0;
        memset(input_buf, 0, sizeof(input_buf));
        msg_scroll_offset = 0;
        load_chat_history(chat_list[active_chat_idx].chat_id);
        redraw_all();
    } else if (strcmp(cmd, "create") == 0) {
        char *name    = strtok_r(NULL, " ",  &saveptr);
        char *members = strtok_r(NULL, "",   &saveptr);
        if (name && members) {
            if (mb_str_width(name) > max_name_width - 4) {
                werase(input_win);
                mvwaddstr(input_win, 0, 0, "Слишком длинное название группы");
                wrefresh(input_win);
                return;
            }
            char sendline[MAX_MSG_LINE];
            build_msg(sendline, sizeof(sendline), CMD_CREATE,
                      "|name=%s|members=%s", name, members);
            send_cmd(sendline);
        }
    } else if (strcmp(cmd, "list") == 0) {
        send_cmd("LIST\n");
    } else if (strcmp(cmd, "help") == 0) {
        help_state = (help_state + 1) % 2;
        redraw_all();
    } else if (strcmp(cmd, "quit") == 0) {
        connected = 0;
    }
}

/* ========== MESSAGE SELECTION (стрелки) ========== */

/*
 * move_selection(delta) — изменяет selected_msg_idx на +delta,
 * и подкручивает msg_scroll_offset так, чтобы выделенное сообщение
 * было видно (в идеале – в верхней трети окна).
 */
static void move_selection(int delta) {
    if (active_chat_idx < 0) return;
    chat_entry_t *chat = &chat_list[active_chat_idx];
    if (chat->msg_count == 0) return;

    int new_sel = chat->selected_msg_idx + delta;
    if (new_sel < 0)               new_sel = chat->msg_count - 1;
    if (new_sel >= chat->msg_count) new_sel = 0;

    /* Сначала просто обновляем индекс и перерисовываем.
       Корректировка скролла делается внутри draw_msg_win (keep_visible). */
    chat->selected_msg_idx = new_sel;
    load_chat_history(chat->chat_id);
}

static int server_id_to_local(const chat_entry_t *chat, int server_id) {
    if (!chat->server_msg_ids || server_id < 0) return -1;
    for (int i = 0; i < chat->msg_count; i++)
        if (chat->server_msg_ids[i] == server_id) return i + 1;
    return -1;
}

/* ========== RENDER ONE MESSAGE → lines[] ========== */

/*
 * rendered_line_t – одна строка сообщения.
 *
 * is_id_line   – содержит номер сообщения (только первая строка).
 * is_me / selected / id_str  – для цветов.
 * text         – сам текст (без номера).
 */
typedef struct {
    char   text[MAX_MSG_LINE];
    int    is_id_line;
    int    is_me;
    int    selected;
    char   id_str[20];      /* "[N]" – только для первой строки */
} rendered_line_t;

static rendered_line_t *render_msg(
        const char *sender, const char *body, int local_id,
        int is_me, int selected,
        int reply_to_local, int fwd_from_server,
        const char *reply_sender,
        int text_max_cols, int *out_count)
{
    /* ---- подготовка тела ---- */
    char clean_body[MAX_BODY];
    const char *src = body;
    char *dst = clean_body;
    while (*src && (dst - clean_body) < (int)(sizeof(clean_body) - 1)) {
        *dst++ = (*src == '\n') ? ' ' : *src;
        src++;
    }
    *dst = '\0';

    /* ---- префикс (ответ / пересылка) ---- */
    char prefix[128] = "";
    if (reply_to_local > 0 && reply_sender && reply_sender[0]) {
        snprintf(prefix, sizeof(prefix), "(ответ на #%d от %s) ",
                 reply_to_local, reply_sender);
    } else if (reply_to_local > 0) {
        snprintf(prefix, sizeof(prefix), "(ответ на #%d) ", reply_to_local);
    } else if (fwd_from_server >= 0) {
        /* Достаём имя отправителя из тела [from ...] */
        char fwd_name[MAX_LOGIN] = "";
        char *fp = strstr(clean_body, "[from ");
        if (fp) {
            char *eb = strchr(fp, ']');
            if (eb) {
                size_t nl = (size_t)(eb - (fp + 6));
                if (nl >= sizeof(fwd_name)) nl = sizeof(fwd_name) - 1;
                memcpy(fwd_name, fp + 6, nl);
                fwd_name[nl] = '\0';
                /* Убираем [from ...] из тела */
                int skip = (int)(eb - fp) + 1;
                if (*(eb + 1) == ' ') skip++;
                memmove(fp, fp + skip, strlen(fp + skip) + 1);
            }
        }
        if (fwd_name[0])
            snprintf(prefix, sizeof(prefix), "(от %s) ", fwd_name);
        else
            snprintf(prefix, sizeof(prefix), "(переслано) ");
    }

    /* ---- полная строка ---- */
    char full_line[MAX_BODY + 256];
    snprintf(full_line, sizeof(full_line), "[%s]: %s%s", sender, prefix, clean_body);

    /* ---- id_str ---- */
    char id_str[20];
    snprintf(id_str, sizeof(id_str), "[%d]", local_id);

    /* ---- разбиваем full_line на куски ---- */
    int capacity = 4;
    rendered_line_t *lines = malloc(capacity * sizeof(rendered_line_t));
    int count = 0;

    const char *p = full_line;
    while (*p) {
        size_t chunk = bytes_for_cols(p, text_max_cols);
        if (chunk == 0) break;

        if (count >= capacity) {
            capacity *= 2;
            lines = realloc(lines, capacity * sizeof(rendered_line_t));
        }

        rendered_line_t *rl = &lines[count];
        memset(rl, 0, sizeof(*rl));
        strncpy(rl->text, p, chunk < MAX_MSG_LINE - 1 ? chunk : MAX_MSG_LINE - 1);
        rl->text[chunk < MAX_MSG_LINE - 1 ? chunk : MAX_MSG_LINE - 1] = '\0';
        rl->is_me    = is_me;
        rl->selected = selected;
        if (count == 0) {
            rl->is_id_line = 1;
            strncpy(rl->id_str, id_str, sizeof(rl->id_str) - 1);
        }
        count++;
        p += chunk;
    }

    if (count == 0) {
        /* пустое сообщение – хотя бы одна пустая строка с номером */
        lines[0] = (rendered_line_t){0};
        lines[0].is_id_line = 1;
        strncpy(lines[0].id_str, id_str, sizeof(lines[0].id_str) - 1);
        count = 1;
    }

    *out_count = count;
    return lines;
}

/* ========== DRAW MSG WINDOW (с авто-прокруткой) ========== */

/*
 * draw_msg_win() рендерит историю чата и автоматически подкручивает
 * msg_scroll_offset так, чтобы selected_idx (если >=0) был виден.
 * При selected_idx == -1 просто показывает хвост истории.
 */
static void draw_msg_win(const char *chat_name,
                         char **senders, char **bodies,
                         int *msg_ids, int *reply_tos, int *fwd_froms,
                         int count, int selected_idx)
{
    int left_margin   = 1;
    int id_col_width  = 6;                  /* ширина "[NNN] " */
    int text_start    = left_margin + id_col_width;
    int text_max_cols = msg_win_w - 2 - text_start;
    if (text_max_cols < 10) text_max_cols = 10;

    /* 1. Рендерим все сообщения в плоский массив строк */
    rendered_line_t **all_lines = malloc(count * sizeof(rendered_line_t *));
    int              *all_counts = malloc(count * sizeof(int));
    int total_lines = 0;

    /* временный указатель для server_id_to_local */
    chat_entry_t *chat = (active_chat_idx >= 0) ? &chat_list[active_chat_idx] : NULL;

    for (int i = 0; i < count; i++) {
        int is_me    = (strcmp(senders[i], my_login) == 0);
        int selected = (i == selected_idx);

        int reply_local = -1;
        if (reply_tos[i] >= 0 && chat)
            reply_local = server_id_to_local(chat, reply_tos[i]);

        const char *reply_sender = NULL;
        if (reply_tos[i] >= 0) {
            for (int j = 0; j < count; j++) {
                if (msg_ids[j] == reply_tos[i]) { reply_sender = senders[j]; break; }
            }
        }

        int lc = 0;
        all_lines[i]  = render_msg(senders[i], bodies[i], i + 1,
                                   is_me, selected,
                                   reply_local, fwd_froms[i], reply_sender,
                                   text_max_cols, &lc);
        all_counts[i] = lc;
        total_lines  += lc;
    }

    /* 2. Вычисляем отступы, чтобы выделенное сообщение было видно */
    int avail = msg_win_h - 2;        /* строки внутри рамки */
    if (avail < 1) avail = 1;

    /* Считаем, сколько строк от начала истории до начала выбранного сообщения */
    int lines_before_selected = 0;
    if (selected_idx >= 0 && selected_idx < count) {
        for (int i = 0; i < selected_idx; i++)
            lines_before_selected += all_counts[i];
    }

    int selected_height = (selected_idx >= 0 && selected_idx < count)
                          ? all_counts[selected_idx] : 0;

    /* Ищем “идеальный” offset:
     * Хотим, чтобы первая строка выделенного сообщения была в верхней четверти окна
     * (target_line = avail / 4), но не даём offset стать отрицательным и не
     * залезаем выше начала истории.
     */
    int target_line = avail / 4;          /* ≈25% от верха */
    int ideal_end_line = lines_before_selected + selected_height + target_line;
    int max_offset = total_lines - avail;
    if (max_offset < 0) max_offset = 0;
    msg_scroll_offset = total_lines - avail - (lines_before_selected - target_line);
    /* Приводим к допустимым границам */
    if (msg_scroll_offset < 0) msg_scroll_offset = 0;
    if (msg_scroll_offset > max_offset) msg_scroll_offset = max_offset;

    /* Если выделение сброшено (selected_idx == -1), просто показываем конец */
    if (selected_idx < 0) msg_scroll_offset = 0;

    int start_line = total_lines - avail - msg_scroll_offset;
    if (start_line < 0) start_line = 0;

    /* 3. Рисуем окно */
    werase(msg_win);
    box(msg_win, 0, 0);
    mvwprintw(msg_win, 0, 1, "Чат: %s", chat_name);

    /* Индикатор скролла */
    if (msg_scroll_offset > 0) {
        wattron(msg_win, COLOR_PAIR(COLOR_INFO));
        mvwprintw(msg_win, 0, msg_win_w - 14, "[^ +%d стр.]", msg_scroll_offset);
        wattroff(msg_win, COLOR_PAIR(COLOR_INFO));
    }

    int win_row = 1;
    int global_line = 0;

    for (int i = 0; i < count && win_row <= avail; i++) {
        for (int li = 0; li < all_counts[i] && win_row <= avail; li++) {
            if (global_line >= start_line) {
                rendered_line_t *rl = &all_lines[i][li];

                /* Номер сообщения (только для первой строки) */
                if (rl->is_id_line) {
                    if (rl->selected) wattron(msg_win, COLOR_PAIR(COLOR_SELECTED));
                    else              wattron(msg_win, COLOR_PAIR(COLOR_ID));
                    mvwprintw(msg_win, win_row, left_margin, "%-*s",
                              id_col_width - 1, rl->id_str);
                    if (rl->selected) wattroff(msg_win, COLOR_PAIR(COLOR_SELECTED));
                    else              wattroff(msg_win, COLOR_PAIR(COLOR_ID));
                }

                /* Текст сообщения */
                if (rl->is_me)   wattron(msg_win, COLOR_PAIR(COLOR_MY_MSG));
                else             wattron(msg_win, COLOR_PAIR(COLOR_OTHER_MSG));
                if (rl->selected) wattron(msg_win, A_REVERSE);

                mvwaddnstr(msg_win, win_row, text_start, rl->text, strlen(rl->text));

                if (rl->is_me)   wattroff(msg_win, COLOR_PAIR(COLOR_MY_MSG));
                else             wattroff(msg_win, COLOR_PAIR(COLOR_OTHER_MSG));
                if (rl->selected) wattroff(msg_win, A_REVERSE);

                win_row++;
            }
            global_line++;
        }
    }

    wrefresh(msg_win);

    /* Освобождаем память */
    for (int i = 0; i < count; i++) free(all_lines[i]);
    free(all_lines);
    free(all_counts);
}

/* ========== LOAD & DISPLAY HISTORY ========== */

static void load_chat_history(int chat_id) {
    if (active_chat_idx < 0) return;
    chat_entry_t *chat = &chat_list[active_chat_idx];

    char **senders, **bodies;
    int *msg_ids, *reply_tos, *fwd_froms;
    int count = local_db_get_messages(chat_id, &senders, &bodies,
                                      &msg_ids, &reply_tos, &fwd_froms, 200);
    chat->msg_count = count;
    free(chat->server_msg_ids);
    chat->server_msg_ids = NULL;
    if (count > 0) {
        chat->server_msg_ids = malloc(count * sizeof(int));
        memcpy(chat->server_msg_ids, msg_ids, count * sizeof(int));
    }
    if (chat->selected_msg_idx >= count) chat->selected_msg_idx = count - 1;

    draw_msg_win(chat->name, senders, bodies, msg_ids, reply_tos, fwd_froms,
                 count, chat->selected_msg_idx);

    local_db_free_messages(count, senders, bodies, msg_ids, reply_tos, fwd_froms);
}

/* ========== MAIN INPUT LOOP ========== */

void process_input(void) {
    if (active_chat_idx >= 0)
        load_chat_history(chat_list[active_chat_idx].chat_id);

    while (connected) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sockfd, &rfds);
        FD_SET(STDIN_FILENO, &rfds);
        struct timeval tv = {0, 50000};
        int maxfd = (sockfd > STDIN_FILENO) ? sockfd : STDIN_FILENO;

        int ready = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (ready < 0) break;

        /* --- обработка входящей очереди --- */
        pthread_mutex_lock(&in_queue.mutex);
        while (in_queue.count > 0) {
            char msg[MAX_MSG_LINE];
            strncpy(msg, in_queue.messages[0].text, MAX_MSG_LINE - 1);
            msg[MAX_MSG_LINE - 1] = '\0';
            memmove(in_queue.messages, in_queue.messages + 1,
                    (in_queue.count - 1) * sizeof(msg_node_t));
            in_queue.count--;
            pthread_mutex_unlock(&in_queue.mutex);

            char cmd[MAX_CMD_LEN];
            get_cmd(msg, cmd, sizeof(cmd));

            if (strcmp(cmd, CMD_MSG)   == 0 || strcmp(cmd, CMD_GRP)   == 0 ||
                strcmp(cmd, CMD_REPLY) == 0 || strcmp(cmd, CMD_FWD)   == 0) {
                char from[MAX_LOGIN], body[MAX_BODY];
                int chat_id, msg_id = -1, reply_to = -1, fwd_from = -1;
                if (get_param(msg, "from", from, sizeof(from)) == 0 &&
                    get_param_int(msg, "chat_id", &chat_id) == 0 &&
                    get_param(msg, "text", body, sizeof(body)) == 0) {
                    get_param_int(msg, "msg_id",   &msg_id);
                    get_param_int(msg, "reply_to", &reply_to);
                    get_param_int(msg, "fwd_from", &fwd_from);

                    if (strcmp(from, my_login) == 0)
                        local_db_confirm_last_msg(chat_id, msg_id);
                    else
                        local_db_save_msg(chat_id, from, msg_id, body, reply_to, fwd_from);

                    int chat_idx = -1;
                    for (int i = 0; i < chat_count; i++)
                        if (chat_list[i].chat_id == chat_id) { chat_idx = i; break; }
                    if (chat_idx == -1) {
                        add_chat(chat_id, from, 0);
                        chat_idx = chat_count - 1;
                        redraw_all();
                    }
                    if (active_chat_idx == chat_idx) {
                        chat_list[chat_idx].msg_count++;
                        /* новое сообщение – скролл в конец, выделить последнее */
                        chat_list[chat_idx].selected_msg_idx = chat_list[chat_idx].msg_count - 1;
                        msg_scroll_offset = 0;
                        load_chat_history(chat_id);
                    }
                }
            } else if (strcmp(cmd, CMD_OK) == 0) {
                int msg_id = -1, chat_id = -1;
                get_param_int(msg, "msg_id",  &msg_id);
                get_param_int(msg, "chat_id", &chat_id);
                if (msg_id >= 0 && active_chat_idx >= 0) {
                    local_db_confirm_last_msg(chat_list[active_chat_idx].chat_id, msg_id);
                    load_chat_history(chat_list[active_chat_idx].chat_id);
                }
                char action[32] = {0};
                if (get_param(msg, "action", action, sizeof(action)) == 0) {
                    if (strcmp(action, "list") == 0) {
                        char users[MAX_MSG_LINE];
                        int ucount;
                        if (get_param(msg, "users", users, sizeof(users)) == 0 &&
                            get_param_int(msg, "count", &ucount) == 0) {
                            if (active_chat_idx >= 0) {
                                chat_entry_t *chat = &chat_list[active_chat_idx];
                                char buf[256];
                                snprintf(buf, sizeof(buf), "=== Онлайн (%d): %s ===", ucount, users);
                                local_db_save_msg(chat->chat_id, "система", -1, buf, -1, -1);
                                load_chat_history(chat->chat_id);
                            }
                        }
                    } else if (strcmp(action, "group_created") == 0) {
                        int gid; char gname[MAX_LOGIN]; int mc = 0;
                        if (get_param_int(msg, "chat_id", &gid) == 0 &&
                            get_param(msg, "name", gname, sizeof(gname)) == 0) {
                            get_param_int(msg, "member_count", &mc);
                            add_chat(gid, gname, 1);
                            for (int i = 0; i < chat_count; i++)
                                if (chat_list[i].chat_id == gid && chat_list[i].is_group) {
                                    chat_list[i].member_count = mc; break;
                                }
                            send_cmd("GET_CHATS\n");
                            redraw_all();
                        }
                    }
                }
                if (chat_id >= 0 && !action[0]) send_cmd("GET_CHATS\n");

            } else if (strcmp(cmd, CMD_ERR) == 0) {
                int code; char desc[MAX_MSG_LINE];
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
                int chat_id = -1; char name[MAX_LOGIN] = {0};
                int is_group = 0, member_count = 0;
                if (get_param_int(msg, "chat_id", &chat_id) == 0 &&
                    get_param(msg, "name", name, sizeof(name)) == 0) {
                    get_param_int(msg, "is_group",     &is_group);
                    get_param_int(msg, "member_count", &member_count);
                    add_chat(chat_id, name, is_group);
                    for (int i = 0; i < chat_count; i++)
                        if (chat_list[i].chat_id == chat_id && chat_list[i].is_group) {
                            chat_list[i].member_count = member_count; break;
                        }
                    char req[MAX_MSG_LINE];
                    build_msg(req, sizeof(req), CMD_HIST, "|chat_id=%d", chat_id);
                    send_cmd(req);
                }
                redraw_all();
            } else {
                /* Формат истории: chat_id|sender|msg_id|body */
                char msg_copy[MAX_MSG_LINE];
                strncpy(msg_copy, msg, MAX_MSG_LINE - 1);
                msg_copy[MAX_MSG_LINE - 1] = '\0';
                char *saveptr;
                char *tok = strtok_r(msg_copy, "|", &saveptr);
                if (tok) {
                    int  hcid    = atoi(tok);
                    char *sender = strtok_r(NULL, "|", &saveptr);
                    char *midstr = strtok_r(NULL, "|", &saveptr);
                    char *hbody  = strtok_r(NULL, "",  &saveptr);
                    if (sender && midstr && hbody) {
                        int mid = atoi(midstr);
                        local_db_save_msg(hcid, sender, mid, hbody, -1, -1);
                        int cidx = -1;
                        for (int i = 0; i < chat_count; i++)
                            if (chat_list[i].chat_id == hcid) { cidx = i; break; }
                        if (cidx == -1) {
                            add_chat(hcid, sender, 0);
                            cidx = chat_count - 1;
                            redraw_all();
                        }
                        if (active_chat_idx == cidx) load_chat_history(hcid);
                    }
                }
            }

            pthread_mutex_lock(&in_queue.mutex);
        }
        pthread_mutex_unlock(&in_queue.mutex);

        /* --- обработка клавиатуры --- */
        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            wint_t wch;
            while (1) {
                int ret = wget_wch(input_win, &wch);
                if (ret == ERR) break;

                if (ret == KEY_CODE_YES) {
                    if (wch == KEY_F(1)) {
                        connected = 0; break;
                    } else if (wch == KEY_UP) {
                        if (input_mode == MODE_NORMAL) move_selection(-1);
                    } else if (wch == KEY_DOWN) {
                        if (input_mode == MODE_NORMAL) move_selection(1);
                    } else if (wch == KEY_PPAGE) {        /* Page Up */
                        msg_scroll_offset += (msg_win_h - 3);
                        if (active_chat_idx >= 0)
                            load_chat_history(chat_list[active_chat_idx].chat_id);
                    } else if (wch == KEY_NPAGE) {        /* Page Down */
                        msg_scroll_offset -= (msg_win_h - 3);
                        if (msg_scroll_offset < 0) msg_scroll_offset = 0;
                        if (active_chat_idx >= 0)
                            load_chat_history(chat_list[active_chat_idx].chat_id);
                    } else if (wch == KEY_BACKSPACE) {
                        if (input_len > 0) input_buf[--input_len] = L'\0';
                        display_input_line();
                    }
                    continue;
                }

                /* Ctrl+U / Ctrl+D — дополнительная прокрутка */
                if (wch == 21) {           /* Ctrl+U */
                    msg_scroll_offset += (msg_win_h - 3);
                    if (active_chat_idx >= 0)
                        load_chat_history(chat_list[active_chat_idx].chat_id);
                    continue;
                }
                if (wch == 4) {            /* Ctrl+D */
                    msg_scroll_offset -= (msg_win_h - 3);
                    if (msg_scroll_offset < 0) msg_scroll_offset = 0;
                    if (active_chat_idx >= 0)
                        load_chat_history(chat_list[active_chat_idx].chat_id);
                    continue;
                }

                if (wch == L'\n') {
                    if (input_mode == MODE_FWD_SELECT_CHAT) {
                        strncpy(pending_fwd_user, chat_list[fwd_candidate_idx].name, MAX_LOGIN - 1);
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
                            int n = wctomb(tmp, input_buf[i]);
                            if (n > 0) strncat(mb_text, tmp, sizeof(mb_text) - strlen(mb_text) - 1);
                        }

                        if (input_mode == MODE_REPLY) {
                            char clean_text[MAX_BODY];
                            sanitize_body(clean_text, mb_text, sizeof(clean_text));
                            if (active_chat_idx >= 0 && chat_list[active_chat_idx].is_group) {
                                char sl[MAX_MSG_LINE];
                                build_msg(sl, sizeof(sl), CMD_GRP,
                                          "|chat_id=%d|text=%s|reply_to=%d",
                                          chat_list[active_chat_idx].chat_id,
                                          clean_text, pending_reply_srv_id);
                                send_cmd(sl);
                                local_db_save_msg(chat_list[active_chat_idx].chat_id,
                                                  my_login, -1, clean_text, pending_reply_srv_id, -1);
                            } else if (active_chat_idx >= 0) {
                                char sl[MAX_MSG_LINE];
                                build_msg(sl, sizeof(sl), CMD_REPLY,
                                          "|to=%s|text=%s|reply_to=%d",
                                          chat_list[active_chat_idx].name,
                                          clean_text, pending_reply_srv_id);
                                send_cmd(sl);
                                local_db_save_msg(chat_list[active_chat_idx].chat_id,
                                                  my_login, -1, clean_text, pending_reply_srv_id, -1);
                            }
                            /* скролл в конец, выделяем последнее */
                            if (active_chat_idx >= 0) {
                                chat_list[active_chat_idx].selected_msg_idx =
                                    chat_list[active_chat_idx].msg_count;
                                msg_scroll_offset = 0;
                                load_chat_history(chat_list[active_chat_idx].chat_id);
                            }
                            input_mode = MODE_NORMAL;
                            pending_reply_srv_id = pending_reply_local = 0;

                        } else if (input_mode == MODE_FWD_TEXT) {
                            char comment_text[MAX_BODY];
                            sanitize_body(comment_text, mb_text, sizeof(comment_text));
                            int dest_idx = -1;
                            for (int i = 0; i < chat_count; i++)
                                if (strcmp(chat_list[i].name, pending_fwd_user) == 0) {
                                    dest_idx = i; break;
                                }
                            if (dest_idx >= 0) {
                                char final_text[MAX_BODY * 2];
                                if (strlen(comment_text) > 0)
                                    snprintf(final_text, sizeof(final_text),
                                             "[from %s] \"%s\" ; %s",
                                             pending_fwd_original_sender,
                                             pending_fwd_original_body, comment_text);
                                else
                                    snprintf(final_text, sizeof(final_text),
                                             "[from %s] \"%s\"",
                                             pending_fwd_original_sender,
                                             pending_fwd_original_body);

                                if (chat_list[dest_idx].is_group) {
                                    char sl[MAX_MSG_LINE];
                                    build_msg(sl, sizeof(sl), CMD_GRP,
                                              "|chat_id=%d|text=%s|fwd_from=%d",
                                              chat_list[dest_idx].chat_id,
                                              final_text, pending_fwd_srv_id);
                                    send_cmd(sl);
                                    local_db_save_msg(chat_list[dest_idx].chat_id,
                                                      my_login, -1, final_text, -1, pending_fwd_srv_id);
                                } else {
                                    char sl[MAX_MSG_LINE];
                                    build_msg(sl, sizeof(sl), CMD_FWD,
                                              "|to=%s|text=%s|fwd_from=%d",
                                              pending_fwd_user, final_text, pending_fwd_srv_id);
                                    send_cmd(sl);
                                    local_db_save_msg(chat_list[dest_idx].chat_id,
                                                      my_login, -1, final_text, -1, pending_fwd_srv_id);
                                }
                                active_chat_idx = dest_idx;
                                chat_list[active_chat_idx].selected_msg_idx =
                                    chat_list[active_chat_idx].msg_count;
                                msg_scroll_offset = 0;
                                load_chat_history(chat_list[active_chat_idx].chat_id);
                            }
                            input_mode = MODE_NORMAL;
                            pending_fwd_srv_id = pending_fwd_local = 0;
                            pending_fwd_user[0] = pending_fwd_original_sender[0] =
                                pending_fwd_original_body[0] = '\0';

                        } else if (mb_text[0] == '/') {
                            process_command(mb_text);
                        } else if (active_chat_idx >= 0) {
                            chat_entry_t *chat = &chat_list[active_chat_idx];
                            char clean_text[MAX_BODY];
                            sanitize_body(clean_text, mb_text, sizeof(clean_text));
                            char sl[MAX_MSG_LINE];
                            if (chat->is_group)
                                build_msg(sl, sizeof(sl), CMD_GRP,
                                          "|chat_id=%d|text=%s", chat->chat_id, clean_text);
                            else
                                build_msg(sl, sizeof(sl), CMD_MSG,
                                          "|to=%s|text=%s", chat->name, clean_text);
                            send_cmd(sl);
                            local_db_save_msg(chat->chat_id, my_login, -1, clean_text, -1, -1);
                            chat->selected_msg_idx = chat->msg_count;
                            msg_scroll_offset = 0;
                            load_chat_history(chat->chat_id);
                        }
                        input_len = 0;
                        memset(input_buf, 0, sizeof(input_buf));
                    }
                    display_input_line();
                    redraw_all();

                } else if (wch == 18) {           /* Ctrl+R */
                    if (active_chat_idx >= 0 && chat_list[active_chat_idx].selected_msg_idx >= 0) {
                        chat_entry_t *chat = &chat_list[active_chat_idx];
                        pending_reply_srv_id = chat->server_msg_ids[chat->selected_msg_idx];
                        pending_reply_local  = chat->selected_msg_idx + 1;
                        input_mode = MODE_REPLY;
                        input_len = 0;
                        memset(input_buf, 0, sizeof(input_buf));
                        display_input_line();
                        redraw_all();
                    }
                } else if (wch == 6) {            /* Ctrl+F */
                    if (active_chat_idx >= 0 && chat_list[active_chat_idx].selected_msg_idx >= 0) {
                        chat_entry_t *chat = &chat_list[active_chat_idx];
                        pending_fwd_srv_id = chat->server_msg_ids[chat->selected_msg_idx];
                        pending_fwd_local  = chat->selected_msg_idx + 1;
                        char orig_sender[MAX_LOGIN] = "unknown";
                        local_db_get_msg_sender(chat->chat_id, pending_fwd_srv_id,
                                                orig_sender, sizeof(orig_sender));
                        strncpy(pending_fwd_original_sender, orig_sender, MAX_LOGIN - 1);
                        char orig_body[MAX_BODY] = "";
                        local_db_get_body_by_msg_id(chat->chat_id, pending_fwd_srv_id,
                                                    orig_body, sizeof(orig_body));
                        sanitize_body(pending_fwd_original_body, orig_body,
                                      sizeof(pending_fwd_original_body));
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
                    } else if (input_mode == MODE_NORMAL && chat_count > 0) {
                        active_chat_idx = (active_chat_idx + 1) % chat_count;
                        input_len = 0;
                        memset(input_buf, 0, sizeof(input_buf));
                        msg_scroll_offset = 0;
                        load_chat_history(chat_list[active_chat_idx].chat_id);
                        redraw_all();
                    }
                } else if (wch >= 32 && input_len < MAX_BODY - 1) {
                    input_buf[input_len++] = wch;
                    input_buf[input_len]   = L'\0';
                    display_input_line();
                }
            }
        }
    }
}