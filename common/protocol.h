#ifndef PROTOCOL_H
#define PROTOCOL_H

/* =========================================================
 * Общий протокол мессенджера
 * Формат сообщений: ТИП|ключ=значение|ключ=значение\n
 * Пример: AUTH|login=vasya|pass=123456\n
 * ========================================================= */

#define PORT_DEFAULT    8080
#define BUF_SIZE        4096
#define MAX_LOGIN       64
#define MAX_TEXT        2048
#define MAX_CHAT_NAME   128
#define MAX_MEMBERS     64

/* Типы сообщений клиент → сервер */
#define MSG_AUTH        "AUTH"   /* AUTH|login=...|pass=...          */
#define MSG_REG         "REG"    /* REG|login=...|pass=...           */
#define MSG_SEND        "MSG"    /* MSG|to=...|text=...              */
#define MSG_GRP         "GRP"    /* GRP|chat_id=...|text=...         */
#define MSG_REPLY       "REPLY"  /* REPLY|...|reply_to=<id>          */
#define MSG_FWD         "FWD"    /* FWD|...|fwd_from=<chat_id>       */
#define MSG_HIST        "HIST"   /* HIST|chat_id=...                 */
#define MSG_CREATE      "CREATE" /* CREATE|name=...|members=a,b,c    */

/* Типы ответов сервер → клиент */
#define RESP_OK         "OK"
#define RESP_ERR        "ERR"

/* Коды ошибок */
#define ERR_BAD_FORMAT  1   /* Неверный формат сообщения   */
#define ERR_NOT_FOUND   2   /* Пользователь не найден      */
#define ERR_OFFLINE     3   /* Адресат не в сети           */
#define ERR_FORBIDDEN   4   /* Доступ к чату запрещён      */
#define ERR_EXISTS      5   /* Пользователь уже существует */
#define ERR_WRONG_PASS  6   /* Неверный пароль             */

#endif /* PROTOCOL_H */
