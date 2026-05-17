CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Wpedantic -g \
           -I./common -I./server -I./client
LDFLAGS = -lpthread

# SQLite и ncurses добавятся на этапах 3 и 7:
# LDFLAGS += -lsqlite3
# LDFLAGS += -lncurses

SRV_SRCS = server/main.c server/network.c server/db.c
CLI_SRCS = client/main.c client/network.c client/ui.c

SRV_OBJS = $(SRV_SRCS:.c=.o)
CLI_OBJS = $(CLI_SRCS:.c=.o)

.PHONY: all clean server client

all: server client

server: $(SRV_OBJS)
	$(CC) $(CFLAGS) -o server_app $^ $(LDFLAGS)

client: $(CLI_OBJS)
	$(CC) $(CFLAGS) -o client_app $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f server/**.o client/**.o server_app client_app
