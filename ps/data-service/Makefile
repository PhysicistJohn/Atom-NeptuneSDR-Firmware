CC ?= cc
PYTHON ?= python3
CPPFLAGS ?=
CFLAGS ?= -O2 -g
WARNINGS := -Wall -Wextra -Werror -Wpedantic
STANDARD := -std=c11
INCLUDES := -Iinclude -I../include -I../../protocol/generated
COMMON_SOURCES := src/packet.c src/dma_ring_linux.c src/dma_ring_mock.c src/udp_sink.c src/service.c
BUILD_DIR := build
SERVICE := $(BUILD_DIR)/neptune-data-service
TEST := $(BUILD_DIR)/test-data-service
ASAN_TEST := $(BUILD_DIR)/test-data-service-asan
EMITTER := $(BUILD_DIR)/emit-packet-fixture
SANITIZER_FLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all test clean

all: $(SERVICE)

$(BUILD_DIR):
	mkdir -p $@

$(SERVICE): $(COMMON_SOURCES) src/main.c include/neptune_data_service.h ../include/neptune_stream_uapi.h ../../protocol/generated/neptune_edge_v1.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(STANDARD) $(INCLUDES) $(COMMON_SOURCES) src/main.c -o $@

$(TEST): $(COMMON_SOURCES) tests/test_data_service.c include/neptune_data_service.h ../include/neptune_stream_uapi.h ../../protocol/generated/neptune_edge_v1.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(STANDARD) $(INCLUDES) $(COMMON_SOURCES) tests/test_data_service.c -o $@

$(ASAN_TEST): $(COMMON_SOURCES) tests/test_data_service.c include/neptune_data_service.h ../include/neptune_stream_uapi.h ../../protocol/generated/neptune_edge_v1.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(WARNINGS) $(STANDARD) $(INCLUDES) $(SANITIZER_FLAGS) $(COMMON_SOURCES) tests/test_data_service.c -o $@

$(EMITTER): src/packet.c tests/emit_packet_fixture.c include/neptune_data_service.h ../../protocol/generated/neptune_edge_v1.h | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) $(STANDARD) $(INCLUDES) src/packet.c tests/emit_packet_fixture.c -o $@

test: $(TEST) $(ASAN_TEST) $(SERVICE) $(EMITTER)
	$(TEST)
	ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 $(ASAN_TEST)
	$(EMITTER) | $(PYTHON) tests/check_packet_fixture.py
	$(SERVICE) --help >/dev/null

clean:
	rm -rf $(BUILD_DIR)
