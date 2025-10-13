CC := gcc
CFLAGS := -Wall -Wextra -Ilibs

markdown.o: source/markdown.c libs/markdown.h libs/document.h
	$(CC) $(CFLAGS) -c source/markdown.c -o markdown.o

command_queue.o: source/command_queue.c libs/command_queue.h
	$(CC) $(CFLAGS) -c source/command_queue.c -o command_queue.o

helper.o: source/helper.c libs/helper.h
	$(CC) $(CFLAGS) -c source/helper.c -o helper.o

all: server client

client: source/client.c markdown.o helper.o libs/client.h
	$(CC) $(CFLAGS) source/client.c markdown.o helper.o -o client

server: source/server.c markdown.o command_queue.o helper.o libs/server.h
	$(CC) $(CFLAGS) source/server.c markdown.o command_queue.o helper.o -o server

clean:
	rm -f *.o client server
