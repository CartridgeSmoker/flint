CC ?= cc

CFLAGS += -std=c11 -Wall -Wextra -Werror -O2 -Wunused-result
CFLAGS += -Igenerated

# Use pkg-config for Wayland/Vulkan flags if available
PKG_CFLAGS := $(shell pkg-config --cflags wayland-client vulkan 2>/dev/null)
PKG_LIBS := $(shell pkg-config --libs wayland-client vulkan 2>/dev/null)

# Wayland flags
WAYLAND_CFLAGS := $(shell pkg-config --cflags wayland-client)
WAYLAND_LIBS := $(shell pkg-config --libs wayland-client)

# Vulkan flags
VULKAN_CFLAGS := $(shell pkg-config --cflags vulkan)
VULKAN_LIBS := $(shell pkg-config --libs vulkan)

# Wayland protocol generation
WAYLAND_PROTOCOLS := /usr/share/wayland-protocols
WAYLAND_SCANNER := wayland-scanner

XDG_XML := $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
XDG_HEADER := generated/xdg-shell-client-protocol.h
XDG_SOURCE := generated/xdg-shell-protocol.c

BIN := bin
SRC := src
OBJ := $(BIN)/app.o $(BIN)/wl.o $(BIN)/vk.o
HDR := include/flint/flint.h src/wl.h src/vk.h

all: bin/flint

generated:
	mkdir -p $@

$(XDG_HEADER): $(XDG_XML) | generated
	$(WAYLAND_SCANNER) client-header $< $@

$(XDG_SOURCE): $(XDG_XML) | generated
	$(WAYLAND_SCANNER) private-code $< $@

bin:
	mkdir -p $@

# Wayland protocol codegen
$(BIN)/wayland-protocols-client.h $(BIN)/wayland-protocols-code.c: \
        /usr/share/wayland-protocols/stable/xdg-shell/xdg-shell.xml
	mkdir -p $(BIN)
	wayland-scanner client-header $< $(BIN)/wayland-protocols-client.h
	wayland-scanner private-code $< $(BIN)/wayland-protocols-code.c

# Object files
$(BIN)/app.o: src/app.c $(HDR)
	$(CC) -c $< -o $@ $(WAYLAND_CFLAGS) $(VULKAN_CFLAGS) -Iinclude

$(BIN)/wl.o: src/wl.c src/wl.h $(BIN)/wayland-protocols-client.h $(BIN)/wayland-protocols-code.c
	$(CC) -c $< -o $@ $(WAYLAND_CFLAGS) -Iinclude -Isrc -Ibin

$(BIN)/vk.o: src/vk.c src/vk.h
	$(CC) -c $< -o $@ $(VULKAN_CFLAGS) -Iinclude -Isrc

$(BIN)/flint: main.c $(OBJ)
	$(CC) $^ -o $@ $(WAYLAND_LIBS) $(VULKAN_LIBS)

check: bin/flint
	./bin/flint

clean:
	rm -rf bin dist generated

# Prevent generated files from being committed
.gitignore
bin/wayland-protocols-client.h
bin/wayland-protocols-code.c

.PHONY: all check clean