# RVM -- a small RV64GC system emulator
#
# Deliberately plain GNU Make: no autotools, no cmake, no pkg-config, so the
# same file drives the desktop build, `make test` in CI and the NDK build
# (android/jni/CMakeLists.txt mirrors the source list).

CC       ?= gcc
AR       ?= ar
CSTD     ?= -std=c11

WARNINGS := -Wall -Wextra -Wshadow -Wundef -Wpointer-arith -Wcast-qual \
            -Wstrict-prototypes -Wmissing-prototypes -Wno-unused-parameter
OPT      ?= -O2 -g
# _POSIX_C_SOURCE unlocks pread/pwrite/ftruncate/nanosleep under strict -std=c11.
DEFS     ?= -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS   ?= $(CSTD) $(DEFS) $(OPT) $(WARNINGS) -Isrc -fno-omit-frame-pointer
LDFLAGS  ?=
LDLIBS   ?= -lm

BUILD    := build
BIN      := rvm
TEST_BIN := tests/rvm_tests

SRC      := $(sort $(shell find src -name '*.c'))
OBJ      := $(patsubst %.c,$(BUILD)/%.o,$(SRC))
DEP      := $(OBJ:.o=.d)

LIB_SRC  := $(filter-out src/main.c,$(SRC))
LIB_OBJ  := $(patsubst %.c,$(BUILD)/%.o,$(LIB_SRC))

TEST_SRC := $(sort $(shell find tests -name '*.c'))
TEST_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(TEST_SRC))

.PHONY: all test lint format clean run dtb coverage help objects
.DEFAULT_GOAL := all

all: $(BIN)

$(BIN): $(OBJ)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)
	@echo "built $@ ($(shell du -h $@ 2>/dev/null | cut -f1))"

# Static library of the core, reused by the test binary and the Android JNI.
$(BUILD)/librvm.a: $(LIB_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

lib: $(BUILD)/librvm.a

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_OBJ) $(BUILD)/librvm.a
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(TEST_OBJ) $(BUILD)/librvm.a $(LDLIBS)

# --------------------------------------------------------------- linting

# -Werror is only applied here, so a normal build stays usable while a
# warning is being triaged.
lint:
	$(CC) $(CSTD) $(DEFS) $(WARNINGS) -Werror -Isrc -fsyntax-only $(SRC) $(TEST_SRC)
	@echo "lint: OK"

format:
	@command -v clang-format >/dev/null || { echo "clang-format not installed"; exit 1; }
	clang-format -i $(SRC) $(TEST_SRC) $(shell find src tests -name '*.h')

# ----------------------------------------------------------------- extras

# Requires device-tree-compiler; the emulator itself builds its DTB in C.
dtb: dtb/rvm.dtb

dtb/rvm.dtb: dtb/rvm.dts
	dtc -I dts -O dtb -o $@ $<

coverage: CFLAGS += --coverage -O0
coverage: clean test

clean:
	rm -rf $(BUILD) $(BIN) $(TEST_BIN) *.gcda *.gcno
	@echo "clean"

run: $(BIN)
	./$(BIN) -m 256 -k vmlinux -d disk.img --create-disk --stats

help:
	@echo "targets: all test lint format dtb coverage clean run"

# Dependency tracking
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(DEP)
