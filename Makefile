CC=gcc
CFLAGS=-Wall -Wextra -O2

SERVER_BIN=server
CLIENT_BIN=client
SERVER_SRC=server.c
CLIENT_SRC=client.c

.PHONY: all server client run-server run-client clean

all: server client

server: $(SERVER_SRC)
	$(CC) $(CFLAGS) -pthread -o $(SERVER_BIN) $(SERVER_SRC)

client: $(CLIENT_SRC)
	$(CC) $(CFLAGS) -o $(CLIENT_BIN) $(CLIENT_SRC)

run-server: server
	./server

run-client: client
	./client

clean:
	rm -f $(SERVER_BIN) $(CLIENT_BIN)