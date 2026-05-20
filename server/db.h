#ifndef SERVER_DB_H
#define SERVER_DB_H

/* =========================================================
 * Модуль работы с SQLite на стороне сервера
 * ========================================================= */

/* Открыть / создать базу данных по пути path.
 * Создаёт таблицы users, chats, chat_members, messages если их нет.
 * Возвращает 0 при успехе, -1 при ошибке. */
int  db_open(const char *path);

/* Закрыть соединение с базой данных. */
void db_close(void);

/* --- Пользователи ---------------------------------------- */

/* Проверить пару login/password_hash. 1 — верно, 0 — нет. */
int  db_auth(const char *login, const char *pass_hash);

/* Зарегистрировать нового пользователя.
 * Возвращает 0 при успехе, -1 если логин уже занят. */
int  db_register(const char *login, const char *pass_hash);

/* --- Сообщения ------------------------------------------- */

/* Сохранить сообщение в таблицу messages.
 * reply_to и fwd_from могут быть 0 (не цитата / не пересылка). */
long long db_save_message(int chat_id, const char *sender,
                          const char *body,
                          long long reply_to, int fwd_from);

/* Получить историю последних limit сообщений чата в виде JSON-строки.
 * Вызывающий обязан освободить память через free(). */
char *db_get_history(int chat_id, int limit);

/* --- Личные диалоги -------------------------------------- */

/* Найти существующий личный чат между двумя пользователями или создать новый.
 * Возвращает chat_id или -1 при ошибке. */
int  db_get_or_create_dialog(const char *login_a, const char *login_b);

/* Получить список всех чатов пользователя в виде форматированной строки.
 * Вызывающий обязан освободить через free(). */
char *db_get_user_chats(const char *login);

/* --- Групповые чаты -------------------------------------- */

/* Создать групповой чат. Возвращает chat_id или -1. */
int  db_create_group(const char *name, const char *members[], int count);

/* Получить список участников группы в массив logins (max_count элементов).
 * Возвращает реальное количество участников. */
int  db_get_members(int chat_id, char logins[][64], int max_count);

/* --- Оффлайн-сообщения ----------------------------------- */

/* Пометить сообщения как доставленные пользователю login. */
void db_mark_delivered(const char *login);

/* Получить недоставленные сообщения для login в виде JSON.
 * Вызывающий обязан освободить память через free(). */
char *db_get_pending(const char *login);

#endif /* SERVER_DB_H */
