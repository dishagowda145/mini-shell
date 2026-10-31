CC     = gcc
CFLAGS = -Wall -Wextra -O2 -g

myshell: myshell.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f myshell

.PHONY: clean
