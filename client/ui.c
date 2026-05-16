#include <stdio.h>
#include <string.h>

#include "ui.h"

/* TODO (этап 7): подключить <ncurses.h> и реализовать весь UI */

void ui_init(void)    { /* TODO */ }
void ui_run(void)     { /* TODO */ }
void ui_cleanup(void) { /* TODO */ }

void ui_append_message(const char *sender, const char *text)
{
    /* Временно — просто вывод в консоль */
    printf("[%s]: %s\n", sender, text);
}

void ui_set_status(const char *msg)
{
    printf("Статус: %s\n", msg);
}
