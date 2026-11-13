CC      ?= gcc
CFLAGS  ?= -Wall -Wextra -O2
LDLIBS  = -lsqlite3 -lcrypto -lm

vendas: vendas.c
	$(CC) $(CFLAGS) -o $@ $< $(LDLIBS)

test: vendas
	./teste.sh

clean:
	rm -f vendas vendas.db
