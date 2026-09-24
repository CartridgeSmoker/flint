CC ?= cc
CXX ?= g++
CFLAGS += -std=c11 -Wall -Wextra -Werror -O2 -Wunused-result
CXXFLAGS += -std=c++17 -Wall -Wextra -Werror -O2 -Wunused-result

# makes sure this program is compiled with the right flags and libraries for Wayland and Vulkan if they are installed on the system
PKG_CFLAGS := $(shell pkg-config --cflags wayland-client vulkan 2>/dev/null)
PKG_LIBS := $(shell pkg-config --libs wayland-client vulkan 2>/dev/null)

all: bin/flint

bin:
	mkdir -p $@

# compiles and links C and C++ sources (wl, vk, and flint)
bin/flint: main.c src/vk.c src/wl.c src/flint.cpp | bin
	$(CXX) $(CXXFLAGS) $(PKG_CFLAGS) -o $@ main.c src/vk.c src/wl.c src/flint.cpp $(PKG_LIBS)

check: bin/flint
	./bin/flint

clean:
	rm -rf bin dist

.PHONY: all check clean
