# Build system for libbcmopen.a (pure C99 static library) and bcmctl (multi-call static CLI)
ZIG        ?= zig
ARM_CC     ?= $(ZIG) cc -target arm-linux-musleabihf -mcpu=cortex_a7
ARM_AR     ?= $(ZIG) ar
HOST_CC    ?= cc

CFLAGS_COMMON := -std=c99 -D_GNU_SOURCE -Wall -Wextra -Iinclude
ARM_CFLAGS    := $(CFLAGS_COMMON) -Os -ffunction-sections -fdata-sections
ARM_LDFLAGS   := -static -Wl,--gc-sections -s
HOST_CFLAGS   := $(CFLAGS_COMMON) -O1 -g

OUT_DIR    ?= ../build
OBJ_DIR    := $(OUT_DIR)/bcmopen-obj

LIB_SRCS   := $(wildcard lib/*.c)
LIB_OBJS   := $(patsubst lib/%.c,$(OBJ_DIR)/%.o,$(LIB_SRCS))
CLI_SRCS   := $(wildcard cli/*.c)
CLI_OBJS   := $(patsubst cli/%.c,$(OBJ_DIR)/cli_%.o,$(CLI_SRCS))

.PHONY: all clean test

all: $(OUT_DIR)/libbcmopen.a $(OUT_DIR)/bcmctl

$(OBJ_DIR):
	@mkdir -p $(OBJ_DIR)

$(OBJ_DIR)/%.o: lib/%.c include/bcmopen.h | $(OBJ_DIR)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

$(OBJ_DIR)/cli_%.o: cli/%.c include/bcmopen.h | $(OBJ_DIR)
	$(ARM_CC) $(ARM_CFLAGS) -c $< -o $@

$(OUT_DIR)/libbcmopen.a: $(LIB_OBJS)
	rm -f $@
	$(ARM_AR) rcs $@ $(LIB_OBJS)

$(OUT_DIR)/bcmctl: $(CLI_OBJS) $(OUT_DIR)/libbcmopen.a
	$(ARM_CC) $(ARM_CFLAGS) $(CLI_OBJS) $(OUT_DIR)/libbcmopen.a $(ARM_LDFLAGS) -o $@

test:
	@mkdir -p $(OBJ_DIR)
	$(HOST_CC) $(HOST_CFLAGS) -DBCMOPEN_HOST_TEST tests/test_m1.c $(LIB_SRCS) -o $(OBJ_DIR)/test_m1
	$(OBJ_DIR)/test_m1

clean:
	rm -rf $(OBJ_DIR) $(OUT_DIR)/libbcmopen.a $(OUT_DIR)/bcmctl
