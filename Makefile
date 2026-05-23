CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -pthread -D_POSIX_C_SOURCE=200809L
LDFLAGS = -lncursesw -lsqlite3

COMMON_DIR = common
SERVER_DIR = server
CLIENT_DIR = client

SERVER_SRCS = $(SERVER_DIR)/main.c $(SERVER_DIR)/network.c $(SERVER_DIR)/db.c
SERVER_OBJS = $(SERVER_SRCS:.c=.o)

CLIENT_SRCS = $(CLIENT_DIR)/main.c $(CLIENT_DIR)/network.c $(CLIENT_DIR)/ui.c $(CLIENT_DIR)/local_db.c
CLIENT_OBJS = $(CLIENT_SRCS:.c=.o)

.PHONY: all clean

all: server_app client_app

server_app: $(SERVER_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

client_app: $(CLIENT_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -I$(COMMON_DIR) -I$(SERVER_DIR) -I$(CLIENT_DIR) -c $< -o $@

clean:
	rm -f $(SERVER_OBJS) $(CLIENT_OBJS) server_app client_app