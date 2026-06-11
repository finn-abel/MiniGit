CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11 -g
INCLUDES = -Iinclude
LDFLAGS =
LDLIBS = -lcrypto
OPENSSL_PREFIX ?= $(firstword $(wildcard /opt/homebrew/opt/openssl@3 /opt/homebrew/opt/openssl /usr/local/opt/openssl@3 /usr/local/opt/openssl))

ifneq ($(OPENSSL_PREFIX),)
INCLUDES += -I$(OPENSSL_PREFIX)/include
LDFLAGS += -L$(OPENSSL_PREFIX)/lib
endif

TARGET = minigit

# Add normal project source files here.
SRC = src/main.c src/commands.c src/commit.c src/fs.c src/hash.c src/index.c src/object.c src/repository.c src/status.c src/tree.c

# Add test source files here.
TEST_SRC = tests/test_commands.c tests/test_commit.c tests/test_fs.c tests/test_hash.c tests/test_index.c tests/test_object.c tests/test_repository.c tests/test_status.c tests/test_tree.c

OBJ = $(SRC:.c=.o)

# Source files needed for tests should not include src/main.c,
# because each test file has its own main function.
TEST_SUPPORT_SRC = $(filter-out src/main.c, $(SRC))
TEST_SUPPORT_OBJ = $(TEST_SUPPORT_SRC:.c=.o)

TEST_TARGETS = $(TEST_SRC:tests/%.c=%)
TEST_OBJ = $(TEST_SRC:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ $^ $(LDLIBS)

%.o: %.c
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

%: tests/%.o $(TEST_SUPPORT_OBJ)
	$(CC) $(CFLAGS) $(INCLUDES) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test: $(TEST_TARGETS)
	@set -e; \
	for test in $(TEST_TARGETS); do \
		echo "Running $$test..."; \
		./$$test; \
		echo ""; \
	done
	@$(MAKE) clean

clean:
	rm -f $(OBJ) $(TEST_SUPPORT_OBJ) $(TEST_OBJ) $(TARGET) $(TEST_TARGETS)
	rm -rf *.dSYM tests/*.dSYM

run: $(TARGET)
	./$(TARGET)

.PHONY: all clean run test
