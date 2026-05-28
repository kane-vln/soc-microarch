CC ?= clang
CPPFLAGS ?= -D_DARWIN_C_SOURCE
CFLAGS ?= -O3 -std=c11 -Wall -Wextra -Werror -pedantic
LDFLAGS ?=

BUILD_DIR := build
SRC_DIR := src
TEST_DIR := tests

HELPERS := $(SRC_DIR)/probe_helpers.c $(SRC_DIR)/probe_helpers.h
TEST_HELPERS := $(BUILD_DIR)/test_probe_helpers
PROBE_BIN := $(BUILD_DIR)/cpu_probes
DEEP_BIN := $(BUILD_DIR)/deep_cpu_probes
ADVANCED_BIN := $(BUILD_DIR)/advanced_cpu_probes

.PHONY: all clean test run-smoke
.PHONY: run-deep-smoke run-deep run-advanced-smoke run-advanced

all: $(PROBE_BIN) $(DEEP_BIN) $(ADVANCED_BIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(TEST_HELPERS): $(TEST_DIR)/test_probe_helpers.c $(HELPERS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(TEST_DIR)/test_probe_helpers.c $(SRC_DIR)/probe_helpers.c -o $@ $(LDFLAGS)

$(PROBE_BIN): $(SRC_DIR)/cpu_probes.c $(HELPERS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC_DIR)/cpu_probes.c $(SRC_DIR)/probe_helpers.c -o $@ $(LDFLAGS)

$(DEEP_BIN): $(SRC_DIR)/deep_cpu_probes.c $(HELPERS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC_DIR)/deep_cpu_probes.c $(SRC_DIR)/probe_helpers.c -o $@ $(LDFLAGS)

$(ADVANCED_BIN): $(SRC_DIR)/advanced_cpu_probes.c $(HELPERS) | $(BUILD_DIR)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC_DIR)/advanced_cpu_probes.c $(SRC_DIR)/probe_helpers.c -o $@ $(LDFLAGS)

test: $(TEST_HELPERS)
	./$(TEST_HELPERS)

run-smoke: $(PROBE_BIN)
	./$(PROBE_BIN) --smoke

run-deep-smoke: $(DEEP_BIN)
	./$(DEEP_BIN) --smoke

run-deep: $(DEEP_BIN)
	./$(DEEP_BIN) --csv

run-advanced-smoke: $(ADVANCED_BIN)
	./$(ADVANCED_BIN) --smoke

run-advanced: $(ADVANCED_BIN)
	./$(ADVANCED_BIN) --csv

clean:
	rm -rf $(BUILD_DIR)
