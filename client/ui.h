#ifndef CLIENT_UI_H
#define CLIENT_UI_H

/* Инициализировать ncurses, создать окна. */
void ui_init(void);

/* Главный цикл обработки ввода. Возвращает когда пользователь выходит. */
void ui_run(void);

/* Добавить сообщение в область чата и перерисовать экран. */
void ui_append_message(const char *sender, const char *text);

/* Показать уведомление об ошибке / статусе. */
void ui_set_status(const char *msg);

/* Освободить ресурсы ncurses. */
void ui_cleanup(void);

#endif /* CLIENT_UI_H */
