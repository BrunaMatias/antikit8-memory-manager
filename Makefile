ifeq ($(origin CC),default)
CC      := gcc
endif
CFLAGS  ?= -std=c99 -Wall -Wextra -pedantic -O2
CPPFLAGS += -Iinclude -Ihardware
AR      ?= ar

HW_SRC  := hardware/hardware_specs.c
SRC     := $(wildcard src/*.c)
LIB_SRC := $(filter-out src/main.c,$(SRC))
BUILD   := build
HEADERS := $(wildcard include/*.h hardware/*.h)
LIB_OBJ := $(patsubst src/%.c,$(BUILD)/%.o,$(LIB_SRC)) $(BUILD)/hardware_specs.o
TESTS   := $(patsubst tests/%.c,$(BUILD)/%,$(wildcard tests/test_*.c))

.PHONY: all test thermal clean

ifneq ($(wildcard src/main.c),)
all: $(BUILD)/simulador
else
# O main/parser pertence ao integrante 3 e ainda nao existe nesta etapa.
all: $(BUILD)/libantikit8.a
endif

$(BUILD)/simulador: $(SRC) $(HW_SRC) $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) $(HW_SRC) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/libantikit8.a: $(LIB_OBJ)
	$(AR) rcs $@ $^

$(BUILD)/%.o: src/%.c $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/hardware_specs.o: $(HW_SRC) hardware/hardware_specs.h | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

$(BUILD)/test_%: tests/test_%.c $(LIB_SRC) $(HW_SRC) $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(LIB_SRC) $(HW_SRC) $(LDFLAGS) $(LDLIBS) -o $@

test: $(TESTS)
	@set -e; for test in $(TESTS); do ./$$test; done

thermal: $(BUILD)/test_thermal
	./$(BUILD)/test_thermal $(BUILD)/thermal_results.csv

$(BUILD):
	mkdir -p $(BUILD)

clean:
	rm -rf $(BUILD)
