CC = gcc
CFLAGS = -Wall -Wextra -O3 -g -pthread -I./include -I./vendor/zlib
LDFLAGS = -L./vendor/zlib -pthread
LDLIBS = -lz

SRC_DIR = src
OBJ_DIR = obj
INC_DIR = include
BIN = pzip

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(OBJ_DIR)/%.o, $(SRCS))

ZLIB_VERSION = 1.3.1
ZLIB_DIR = vendor/zlib
ZLIB_STATIC = $(ZLIB_DIR)/libz.a
ZLIB_TAR = vendor/zlib-$(ZLIB_VERSION).tar.gz
ZLIB_URL = https://github.com/madler/zlib/archive/refs/tags/v$(ZLIB_VERSION).tar.gz

.PHONY: all clean distclean test

all: $(ZLIB_STATIC) $(BIN)

# Setup and build zlib dependency locally
$(ZLIB_STATIC):
	@mkdir -p vendor
	@if [ ! -f $(ZLIB_STATIC) ]; then \
		echo "Downloading zlib v$(ZLIB_VERSION)..."; \
		curl -L -o $(ZLIB_TAR) $(ZLIB_URL) || wget -O $(ZLIB_TAR) $(ZLIB_URL); \
		echo "Extracting zlib..."; \
		tar -xf $(ZLIB_TAR) -C vendor; \
		mv vendor/zlib-$(ZLIB_VERSION) $(ZLIB_DIR); \
		rm -f $(ZLIB_TAR); \
		echo "Configuring and building zlib statically..."; \
		(cd $(ZLIB_DIR) && ./configure --static && $(MAKE)); \
	fi

$(BIN): $(OBJS)
	$(CC) $(OBJS) -o $@ $(LDFLAGS) $(ZLIB_STATIC)

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

clean:
	rm -rf $(OBJ_DIR) $(BIN)

distclean: clean
	rm -rf vendor
