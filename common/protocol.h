/*
 * protocol.h – общие определения протокола обмена.
 * Используется и клиентом, и сервером.
 *
 * Формат сообщения:
 *   КОМАНДА|параметр1=значение1|параметр2=значение2|...\n
 *
 * Примеры:
 *   AUTH|login=alex|pass=123\n
 *   MSG|to=alex|text=Привет!\n
 *   OK|msg_id=42\n
 *   ERR|code=2|desc=Пользователь не найден\n
 */

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

/* ------------------------- Типы сообщений ------------------------- */
#define CMD_AUTH      "AUTH"      /* авторизация */
#define CMD_REG       "REG"       /* регистрация */
#define CMD_MSG       "MSG"       /* личное сообщение */
#define CMD_GRP       "GRP"       /* групповое сообщение */
#define CMD_REPLY     "REPLY"     /* ответ на сообщение (цитирование) */
#define CMD_FWD       "FWD"       /* пересылка сообщения */
#define CMD_HIST      "HIST"      /* запрос истории чата */
#define CMD_CREATE    "CREATE"    /* создание группового чата */
#define CMD_OK        "OK"        /* успешный ответ */
#define CMD_ERR       "ERR"       /* ответ с ошибкой */
#define CMD_LIST      "LIST"      /* запрос списка онлайн-пользователей */
#define CMD_HELP      "HELP"      /* запрос справки */
#define CMD_GET_CHATS "GET_CHATS" /* запрос списка чатов пользователя */

/* ------------------------- Коды ошибок ------------------------- */
#define ERR_FORMAT   1   /* неверный формат сообщения */
#define ERR_NOUSER   2   /* пользователь не найден */
#define ERR_OFFLINE  3   /* адресат не в сети */
#define ERR_ACCESS   4   /* доступ запрещён */

/* ------------------------- Ограничения ------------------------- */
#define MAX_LOGIN     32    /* макс. длина логина */
#define MAX_PASS      64    /* макс. длина пароля */
#define MAX_BODY     1024   /* макс. длина текста сообщения */
#define MAX_MSG_LINE 2048   /* макс. длина строки протокола */
#define MAX_CMD_LEN   16    /* макс. длина команды (AUTH, MSG, …) */

/* Порт по умолчанию, на котором сервер слушает подключения */
#define DEFAULT_PORT 12345

/* ================================================================
 * build_msg – собирает строку протокола и дописывает '\n'
 *
 * Параметры:
 *   buf  – выходной буфер
 *   size – размер буфера
 *   cmd  – команда (например, "MSG")
 *   fmt  – форматная строка для параметров (например, "|to=%s|text=%s")
 *   ...  – аргументы для форматной строки
 *
 * Возвращает длину собранной строки (без '\0') или -1 при ошибке.
 *
 * Пример:
 *   build_msg(buf, sizeof(buf), "MSG", "|to=%s|text=%s", "alex", "Hi");
 *   // buf = "MSG|to=alex|text=Hi\n"
 * ================================================================ */
static inline int build_msg(char *buf, size_t size, const char *cmd,
                            const char *fmt, ...) {
    /* записываем команду */
    int len = snprintf(buf, size, "%s", cmd);
    if (len < 0 || (size_t)len >= size) return -1;

    /* если есть дополнительные параметры – дописываем их */
    if (fmt && fmt[0]) {
        va_list ap;
        va_start(ap, fmt);
        int add = vsnprintf(buf + len, size - len, fmt, ap);
        va_end(ap);
        if (add < 0 || (size_t)(len + add) >= size) return -1;
        len += add;
    }

    /* добавляем завершающий перевод строки, если есть место */
    if ((size_t)len + 1 < size) {
        buf[len++] = '\n';
        buf[len] = '\0';
    } else {
        return -1;
    }
    return len;
}

/* ================================================================
 * get_param – извлекает значение параметра key=value из строки msg
 *
 * Возвращает 0 при успехе, -1 если параметр не найден.
 *
 * Пример:
 *   char login[32];
 *   get_param("AUTH|login=alex|pass=123", "login", login, sizeof(login));
 *   // login = "alex"
 * ================================================================ */
static inline int get_param(const char *msg, const char *key,
                            char *value, size_t val_size) {
    /* формируем строку поиска "key=" */
    char search[64];
    snprintf(search, sizeof(search), "%s=", key);

    /* ищем начало "key=" */
    const char *start = strstr(msg, search);
    if (!start) return -1;

    /* переходим к значению (сразу за '=') */
    start += strlen(search);

    /* ищем конец значения – символ '|' или конец строки */
    const char *end = strpbrk(start, "|\n\r");
    size_t len = end ? (size_t)(end - start) : strlen(start);

    /* обрезаем, если не влезает в буфер */
    if (len >= val_size) len = val_size - 1;

    memcpy(value, start, len);
    value[len] = '\0';
    return 0;
}

/* ================================================================
 * get_param_int – как get_param, но преобразует значение в int
 * ================================================================ */
static inline int get_param_int(const char *msg, const char *key, int *val) {
    char buf[32];
    if (get_param(msg, key, buf, sizeof(buf)) == 0) {
        *val = atoi(buf);
        return 0;
    }
    return -1;
}

/* ================================================================
 * get_cmd – извлекает команду (первые символы до '|')
 *
 * Пример:
 *   char cmd[16];
 *   get_cmd("MSG|to=alex|text=Hi\n", cmd, sizeof(cmd));
 *   // cmd = "MSG"
 * ================================================================ */
static inline void get_cmd(const char *msg, char *cmd, size_t size) {
    /* ищем первый '|' */
    const char *end = strchr(msg, '|');
    size_t len = end ? (size_t)(end - msg) : strlen(msg);

    /* убираем возможный '\n' или '\r' в конце команды */
    if (len > 0 && (msg[len-1] == '\n' || msg[len-1] == '\r')) len--;

    /* обрезаем, если не влезает */
    if (len >= size) len = size - 1;

    memcpy(cmd, msg, len);
    cmd[len] = '\0';
}

#endif