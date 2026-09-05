CC = gcc
CFLAGS = -Wall -Wextra -std=c11

financas: financas.c
	$(CC) $(CFLAGS) financas.c -o financas

clean:
	rm -f financas
