# Helix — top-level Makefile.
#
# Targets:
#   make            build static library
#   make test       build and run unit tests
#   make example    build the order-aggregate example
#   make clean      remove build artifacts

CC      ?= cc
AR      ?= ar
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes \
           -fno-strict-aliasing -D_POSIX_C_SOURCE=200809L
LDFLAGS ?=
LDLIBS  ?= -lpthread

INCLUDES := -Iinclude

BUILD_DIR := build
SRC_DIR   := src
TEST_DIR  := tests
EX_DIR    := examples

SRCS := $(shell find $(SRC_DIR) -name '*.c')
OBJS := $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))

LIB := $(BUILD_DIR)/libhelix.a

.PHONY: all clean test example

all: $(LIB)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# Pattern rule needs the destination subdirectory to exist; mkdir -p is cheap.
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

$(LIB): $(OBJS)
	$(AR) rcs $@ $^

# --- tests ---
TEST_SRCS := $(wildcard $(TEST_DIR)/*.c)
TEST_BINS := $(patsubst $(TEST_DIR)/%.c,$(BUILD_DIR)/test_%,$(TEST_SRCS))

$(BUILD_DIR)/test_%: $(TEST_DIR)/%.c $(LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

test: $(TEST_BINS)
	@status=0; \
	for t in $(TEST_BINS); do \
	  echo "--- $$t"; \
	  $$t || status=1; \
	done; \
	exit $$status

# --- example ---
EX_SRCS := $(wildcard $(EX_DIR)/*.c)
EX_BINS := $(patsubst $(EX_DIR)/%.c,$(BUILD_DIR)/example_%,$(EX_SRCS))

$(BUILD_DIR)/example_%: $(EX_DIR)/%.c $(LIB) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(INCLUDES) $< $(LIB) $(LDFLAGS) $(LDLIBS) -o $@

example: $(EX_BINS)

clean:
	rm -rf $(BUILD_DIR) data
