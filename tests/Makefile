# Host-side unit tests for the Quatra Arkansas library.
#
# The library is pure C (PRD v1.1 amendments §2) so it compiles on macOS
# and Linux without any embedded toolchain. cJSON is vendored under
# tests/vendor/, so no system-wide library install is required.
#
#   make test         # build + run all three suites
#   make test-et0
#   make test-zone
#   make test-uart
#   make clean

ROOT     := ..
INCLUDE  := $(ROOT)/components/quatra/include
SRC      := $(ROOT)/components/quatra/src
VENDOR   := vendor
BUILD    := build

CC       ?= cc
CFLAGS   ?= -std=c99 -O2 -Wall -Wextra -Wno-unused-parameter \
            -Wno-unused-function -Wno-deprecated-declarations \
            -I$(INCLUDE) -I. -I$(VENDOR)
LDLIBS   ?= -lm

ET0_OBJS  := $(BUILD)/quatra_et0.o
ZONE_OBJS := $(BUILD)/quatra_zone.o
UART_OBJS := $(BUILD)/quatra_uart.o $(BUILD)/cJSON.o

.PHONY: all test test-et0 test-zone test-uart clean

all: $(BUILD)/test_et0 $(BUILD)/test_zone $(BUILD)/test_uart

test: test-et0 test-zone test-uart

test-et0: $(BUILD)/test_et0
	@$(BUILD)/test_et0

test-zone: $(BUILD)/test_zone
	@$(BUILD)/test_zone

test-uart: $(BUILD)/test_uart
	@$(BUILD)/test_uart

$(BUILD)/test_et0: $(ET0_OBJS) $(BUILD)/test_et0.o | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test_zone: $(ZONE_OBJS) $(BUILD)/test_zone.o | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/test_uart: $(UART_OBJS) $(BUILD)/test_uart.o | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/%.o: $(SRC)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/cJSON.o: $(VENDOR)/cJSON.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD):
	@mkdir -p $(BUILD)

clean:
	@rm -rf $(BUILD)
