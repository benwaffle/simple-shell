CFLAGS += -Wall -Wextra -Werror -pedantic -std=c99 -g

sish: sish.c parse.c run.c
	$(CC) $(CFLAGS) -o $@ $^ $>

clean:
	rm sish

.PHONY: clean
