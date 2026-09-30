CC ?= cc
CFLAGS += -std=c11 -Wall -Wextra -Werror -O2 -Wunused-result

# Use pkg-config for Wayland/Vulkan flags if available
PKG_CFLAGS := $(shell pkg-config --cflags wayland-client vulkan 2>/dev/null)
PKG_LIBS := $(shell pkg-config --libs wayland-client vulkan 2>/dev/null)

all: bin/flint

bin:
	mkdir -p $@

bin/flint: main.c src/vk.c src/wl.c src/flint.c | bin
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -o $@ main.c src/vk.c src/wl.c src/flint.c $(PKG_LIBS)

check: bin/flint
	./bin/flint

clean:
	rm -rf bin dist

.PHONY: all check clean
