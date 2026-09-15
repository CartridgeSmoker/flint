CC ?= cc
CFLAGS += -std=c11 -Wall -Wextra -Werror -O2 -Wunused-result

all: bin/flint

bin:
	mkdir -p $@

bin/flint: main.c | bin
	$(CC) $(CFLAGS) -o $@ $^

check: bin/flint
	./bin/flint

clean:
	rm -rf bin dist

.PHONY: all check clean
