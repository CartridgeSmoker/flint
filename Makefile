CC ?= cc
CXX ?= c++

CFLAGS += -std=c23 -pedantic -Wall -Wextra -Werror
CFLAGS += -O2 -Wunused-result
CFLAGS += -Igenerated -Iinclude

# C++ sources build as C++23. -Iinclude
# lets src/ui.cpp find flint/ui.h, -I.
# covers the include/flint/ui.h spelling.
CXXFLAGS += -std=c++23 -pedantic -Wall -Wextra
CXXFLAGS += -Werror -O2
CXXFLAGS += -Igenerated -Iinclude -I.

# Use pkg-config for Wayland/Vulkan flags if available
PKG_CFLAGS := $(shell pkg-config --cflags wayland-client vulkan 2>/dev/null)
PKG_LIBS := $(shell pkg-config --libs wayland-client vulkan 2>/dev/null)

# Wayland protocol generation
WAYLAND_PROTOCOLS := /usr/share/wayland-protocols
WAYLAND_SCANNER := wayland-scanner

XDG_XML := $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
XDG_HEADER := generated/xdg-shell-client-protocol.h
XDG_SOURCE := generated/xdg-shell-protocol.c

# Explicit C sources. The generated
# protocol source builds as C.
C_SRCS := main.c src/vk.c src/wl.c
C_SRCS += src/flint.c $(XDG_SOURCE)

# Explicit C++ sources. $(wildcard) keeps
# make working before src/ui.cpp lands;
# it expands empty until the file exists.
CXX_SRCS := $(wildcard src/ui.cpp)

C_OBJS := $(C_SRCS:.c=.o)
CXX_OBJS := $(CXX_SRCS:.cpp=.o)
OBJS := $(C_OBJS) $(CXX_OBJS)

all: bin/flint

generated:
	mkdir -p $@

$(XDG_HEADER): $(XDG_XML) | generated
	$(WAYLAND_SCANNER) client-header $< $@

$(XDG_SOURCE): $(XDG_XML) | generated
	$(WAYLAND_SCANNER) private-code $< $@

bin:
	mkdir -p $@

# C objects.
%.o: %.c $(XDG_HEADER)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -c $< -o $@

# C++ objects.
%.o: %.cpp
	$(CXX) $(CXXFLAGS) $(PKG_CFLAGS) -c $< -o $@

# Link with $(CXX) so C++ objects resolve
# libstdc++ without extra flags.
bin/flint: $(OBJS) | bin
	$(CXX) -o $@ $(OBJS) $(PKG_LIBS)

check: bin/flint
	./bin/flint --frames 120

clean:
	rm -f main.o src/*.o
	rm -rf bin dist generated

.PHONY: all check clean
