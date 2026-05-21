#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

// Типы сообщений
#define CMD_AUTH      "AUTH"
#define CMD_REG       "REG"
#define CMD_MSG       "MSG"
#define CMD_GRP       "GRP"
#define CMD_REPLY     "REPLY"
#define CMD_FWD       "FWD"
#define CMD_HIST      "HIST"
#define CMD_CREATE    "CREATE"
#define CMD_OK        "OK"
#define CMD_ERR       "ERR"
#define CMD_LIST      "LIST"
#define CMD_HELP      "HELP"
#define CMD_GET_CHATS "GET_CHATS"

// Коды ошибок
#define ERR_FORMAT   1
#define ERR_NOUSER   2
#define ERR_OFFLINE  3
#define ERR_ACCESS   4

// Ограничения
#define MAX_LOGIN     32
#define MAX_PASS      64
#define MAX_BODY     1024
#define MAX_MSG_LINE 2048
#define MAX_CMD_LEN   16

// Порт по умолчанию
#define DEFAULT_PORT 12345

static inline int build_msg(char *buf, size_t size, const char *cmd,
                            const char *fmt, ...) {
    int len = snprintf(buf, size, "%s", cmd);
    if (len < 0 || (size_t)len >= size) return -1;
    if (fmt && fmt[0]) {
        va_list ap;
        va_start(ap, fmt);
        int add = vsnprintf(buf + len, size - len, fmt, ap);
        va_end(ap);
        if (add < 0 || (size_t)(len + add) >= size) return -1;
        len += add;
    }
    if ((size_t)len + 1 < size) {
        buf[len++] = '\n';
        buf[len] = '\0';
    } else {
        return -1;
    }
    return len;
}

static inline int get_param(const char *msg, const char *key,
                            char *value, size_t val_size) {
    char search[64];
    snprintf(search, sizeof(search), "%s=", key);
    const char *start = strstr(msg, search);
    if (!start) return -1;
    start += strlen(search);
    const char *end = strpbrk(start, "|\n\r");
    size_t len = end ? (size_t)(end - start) : strlen(start);
    if (len >= val_size) len = val_size - 1;
    memcpy(value, start, len);
    value[len] = '\0';
    return 0;
}

static inline int get_param_int(const char *msg, const char *key, int *val) {
    char buf[32];
    if (get_param(msg, key, buf, sizeof(buf)) == 0) {
        *val = atoi(buf);
        return 0;
    }
    return -1;
}

static inline void get_cmd(const char *msg, char *cmd, size_t size) {
    const char *end = strchr(msg, '|');
    size_t len = end ? (size_t)(end - msg) : strlen(msg);
    if (len > 0 && (msg[len-1] == '\n' || msg[len-1] == '\r')) len--;
    if (len >= size) len = size - 1;
    memcpy(cmd, msg, len);
    cmd[len] = '\0';
}

#endif