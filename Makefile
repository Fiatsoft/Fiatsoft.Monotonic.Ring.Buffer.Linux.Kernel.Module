KDIR := /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

MAX_DEVICES ?= 64

# ---------------------------------------------------
# Directories
# ---------------------------------------------------
SRC_DIR        := src
KERNEL_DIR     := $(SRC_DIR)/kernel
USER_DIR       := $(SRC_DIR)/user
COMMON_DIR     := $(SRC_DIR)/common

# src := $(KERNEL_DIR)

USER_COMMON    := $(USER_DIR)/common
USER_TESTS     := $(USER_DIR)/tests
USER_TOOLS     := $(USER_DIR)/tools

BUILD_DIR      := build
KBUILD_DIR     := $(BUILD_DIR)/kernel
UBUILD_DIR     := $(BUILD_DIR)/user

BIN_TESTS      := $(UBUILD_DIR)/tests
BIN_TOOLS      := $(UBUILD_DIR)/tools
BIN_COMMON     := $(UBUILD_DIR)/common

# ---------------------------------------------------
# Kernel module
# ---------------------------------------------------
obj-m += ringbuf.o

ringbuf-objs := \
	$(KERNEL_DIR)/ringbuf_core.o \
	$(KERNEL_DIR)/ringbuf_debugfs.o \
	$(KERNEL_DIR)/ringbuf_abi.o
	
ccflags-y := -g -Og -Wall -Wextra -Werror \
             -DMAX_DEVICES=$(MAX_DEVICES) \
             -I$(COMMON_DIR) \
             -I$(KERNEL_DIR)/include

# ---------------------------------------------------
# User-space flags
# ---------------------------------------------------
CFLAGS += -g -Og -Wall -Wextra -Wno-unused-result -Werror \
          -I$(COMMON_DIR) \
          -I$(USER_COMMON)

# ---------------------------------------------------
# Auto-discovery
# ---------------------------------------------------
TEST_SRCS := $(wildcard $(USER_TESTS)/*.c)
TOOL_SRCS := $(wildcard $(USER_TOOLS)/*.c)

TEST_BINS := $(patsubst $(USER_TESTS)/%.c,$(BIN_TESTS)/%,$(TEST_SRCS))
TOOL_BINS := $(patsubst $(USER_TOOLS)/%.c,$(BIN_TOOLS)/%,$(TOOL_SRCS))

# Shared objects
COMMON_OBJS := \
	$(BIN_COMMON)/helpers.o \
	$(BIN_COMMON)/ringbuf_abi.o

# ---------------------------------------------------
# Targets
# ---------------------------------------------------
.PHONY: all module user tests tools clean dirs

all: module user

user: dirs $(TEST_BINS) $(TOOL_BINS)
	@echo "User binaries built."

tests: $(TEST_BINS)
tools: $(TOOL_BINS)

# ---------------------------------------------------
# Kernel build
# ---------------------------------------------------
module:
	@echo "Building kernel module..."
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	@mkdir -p $(KBUILD_DIR)
	@mv -f *.ko $(KBUILD_DIR)/ 2>/dev/null || true

# ---------------------------------------------------
# Directory creation
# ---------------------------------------------------
dirs:
	@mkdir -p $(KBUILD_DIR)
	@mkdir -p $(BIN_TESTS)
	@mkdir -p $(BIN_TOOLS)
	@mkdir -p $(BIN_COMMON)

# ---------------------------------------------------
# Common objects
# ---------------------------------------------------
$(BIN_COMMON)/helpers.o: $(USER_COMMON)/helpers.c | dirs
	@echo "Compiling helpers.c"
	$(CC) $(CFLAGS) -c $< -o $@

$(BIN_COMMON)/ringbuf_abi.o: $(USER_COMMON)/ringbuf_abi.c | dirs
	@echo "Compiling ringbuf_abi.c (user)"
	$(CC) $(CFLAGS) -c $< -o $@

# ---------------------------------------------------
# Tests
# ---------------------------------------------------
$(BIN_TESTS)/%: $(USER_TESTS)/%.c $(COMMON_OBJS) | dirs
	@echo "Building test $@"
	$(CC) $(CFLAGS) -o $@ $^

# ---------------------------------------------------
# Tools
# ---------------------------------------------------
$(BIN_TOOLS)/%: $(USER_TOOLS)/%.c $(COMMON_OBJS) | dirs
	@echo "Building tool $@"
	$(CC) $(CFLAGS) -o $@ $^

# ---------------------------------------------------
# Scripts (chmod only)
# ---------------------------------------------------
scripts:
	chmod +x $(USER_TESTS)/*.sh 2>/dev/null || true

# ---------------------------------------------------
# Clean
# ---------------------------------------------------
clean:
	@echo "Cleaning build artifacts..."
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -rf $(BUILD_DIR)