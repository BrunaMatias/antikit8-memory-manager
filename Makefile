ifeq ($(origin CC),default)
CC      := gcc
endif
CFLAGS  ?= -std=c99 -Wall -Wextra -pedantic -O2
CFLAGS  += -Iinclude -Ihardware

HW_SRC  := hardware/hardware_specs.c
SRC     := $(wildcard src/*.c)
LIB_SRC := $(filter-out src/main.c,$(SRC))
BUILD   := build

.PHONY: all test clean

all: $(BUILD)/simulador

$(BUILD)/simulador: $(SRC) $(HW_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/test_%: tests/test_%.c $(LIB_SRC) $(HW_SRC) | $(BUILD)
	$(CC) $(CFLAGS) $^ -o $@

test: $(BUILD)/test_address $(BUILD)/test_memory
	./$(BUILD)/test_address
	./$(BUILD)/test_memory

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
