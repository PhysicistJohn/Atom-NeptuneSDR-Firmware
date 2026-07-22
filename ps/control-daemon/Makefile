CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O2 -g
WARNINGS := -Wall -Wextra -Werror -Wpedantic
STANDARD := -std=c11
INCLUDES := -Iinclude -I../include -I../../protocol/generated
ifneq ($(shell uname -s),Linux)
INCLUDES += -I../kernel/neptune_stream/tests/include
endif
COMMON_SOURCES := src/necp.c src/backend_mock.c src/backend_linux.c src/transport.c
BUILD_DIR := build
DAEMON := $(BUILD_DIR)/neptune-control-daemon
TEST := $(BUILD_DIR)/test-necp

.PHONY: all test clean

all: $(DAEMON)

$(BUILD_DIR):
	mkdir -p $@

$(DAEMON): $(COMMON_SOURCES) src/main.c include/necp_daemon.h ../include/neptune_pl_registers_v1.h ../include/neptune_stream_uapi.h ../../protocol/generated/neptune_edge_v1.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(STANDARD) $(INCLUDES) $(COMMON_SOURCES) src/main.c -o $@

$(TEST): $(COMMON_SOURCES) tests/test_necp.c include/necp_daemon.h ../include/neptune_pl_registers_v1.h ../include/neptune_stream_uapi.h ../../protocol/generated/neptune_edge_v1.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(STANDARD) $(INCLUDES) $(COMMON_SOURCES) tests/test_necp.c -o $@

test: $(TEST) $(DAEMON)
	$(TEST)
	$(DAEMON) --help >/dev/null

clean:
	rm -rf $(BUILD_DIR)
