CC      = gcc

# Derive version from git: "v1.0-5-gabcdef" → "v1.0-5"; fallback to "dev"
GIT_DESCRIBE := $(shell git describe --tags --long --always 2>/dev/null)
GIT_VERSION  := $(shell echo "$(GIT_DESCRIBE)" | sed 's/-g[0-9a-f]*$$//')
ifeq ($(strip $(GIT_VERSION)),)
GIT_VERSION  := dev
endif

CFLAGS  = -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200112L \
          -DGIT_VERSION='"$(GIT_VERSION)"' \
          -Isrc $(shell pkg-config --cflags libcurl openssl 2>/dev/null)
LDFLAGS = $(shell pkg-config --libs libcurl openssl 2>/dev/null || echo "-lcurl -lssl -lcrypto") \
          -lpthread

SRCS = src/main.c \
       src/control.c \
       src/connection.c \
       src/pretest.c \
       src/tests.c \
       src/uuid_store.c

OBJS = $(SRCS:src/%.c=build/%.o)
BIN  = build/rmbt-client

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

clean:
	rm -rf build

install: $(BIN)
	install -m 755 $(BIN) /usr/local/bin/rmbt-client

.PHONY: all clean install
