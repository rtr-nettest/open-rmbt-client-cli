CC      = gcc

# Version strings from git.
#   GIT_VERSION_FULL: full describe with hash, e.g. "v2.1-4-ged32b3d" → client_version
#   GIT_REVISION:     describe suffixed with branch, e.g. "v2.1-4-ged32b3d-dev"
#                     → softwareVersion/softwareVersionName/softwareRevision
#                     (no suffix if the branch is unknown); fallback "dev".
GIT_DESCRIBE     := $(shell git describe --tags --always 2>/dev/null)
GIT_BRANCH       := $(shell git rev-parse --abbrev-ref HEAD 2>/dev/null)
ifeq ($(strip $(GIT_DESCRIBE)),)
GIT_DESCRIBE     := dev
endif
GIT_VERSION_FULL := $(GIT_DESCRIBE)
ifeq ($(strip $(GIT_BRANCH)),)
GIT_REVISION     := $(GIT_DESCRIBE)
else
GIT_REVISION     := $(GIT_DESCRIBE)-$(GIT_BRANCH)
endif

CFLAGS  = -Wall -Wextra -O2 -std=c11 -D_POSIX_C_SOURCE=200112L \
          -DGIT_VERSION_FULL='"$(GIT_VERSION_FULL)"' \
          -DGIT_REVISION='"$(GIT_REVISION)"' \
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
