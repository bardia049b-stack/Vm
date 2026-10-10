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
# WERROR=1 turns the whole build into a lint pass; `make lint` always does.
ifeq ($(WERROR),1)
WARNINGS += -Werror
endif
# Build mode.  MODE=debug is what you want under gdb or with --trace; MODE=
# release is what CI ships.  An explicit OPT= on the command line still wins.
MODE     ?= release
ifeq ($(MODE),debug)
OPT      ?= -O0 -g3 -DRVM_DEBUG
else ifeq ($(MODE),sanity)
OPT      ?= -O1 -g -fsanitize=address,undefined
else
# No -g in the shipped build: the debug info alone weighed more than the whole
# program (349 KB against 90 KB), and PLAN caps the desktop binary at 300 KB.
# MODE=debug is one make away when something needs a line number.
OPT      ?= -O2
endif
# _POSIX_C_SOURCE unlocks pread/pwrite/ftruncate/nanosleep under strict -std=c11.
DEFS     ?= -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS   ?= $(CSTD) $(DEFS) $(OPT) $(WARNINGS) -Isrc -fno-omit-frame-pointer
# EXTRA_CFLAGS / EXTRA_LDFLAGS are appended last, so they can override -O.
CFLAGS   += $(EXTRA_CFLAGS)
LDFLAGS  ?=
# MODE=sanity needs the sanitizer runtime at link time too.
ifeq ($(MODE),sanity)
LDFLAGS  += -fsanitize=address,undefined
endif
LDFLAGS  += $(EXTRA_LDFLAGS)
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
	@echo "built $@ ($$(du -h $@ 2>/dev/null | cut -f1))"

# Static library of the core, reused by the test binary and the Android JNI.
$(BUILD)/librvm.a: $(LIB_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

lib: $(BUILD)/librvm.a

# TEST_ARGS goes straight to the runner, e.g. `make test TEST_ARGS=-v`.
TEST_ARGS ?=

test: $(TEST_BIN)
	./$(TEST_BIN) $(TEST_ARGS)

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

# Two sequential sub-makes, not `coverage: clean test`.  With clean as a
# prerequisite, -j runs it concurrently with the compiles and deletes build/
# out from under them ("cannot open build/.../harness.gcno").  -j is inherited
# through MAKEFLAGS, so the rebuild is still parallel.
coverage:
	@$(MAKE) --no-print-directory clean
	@$(MAKE) --no-print-directory test EXTRA_CFLAGS='--coverage -O0' EXTRA_LDFLAGS='--coverage'

clean:
	rm -rf $(BUILD) $(BIN) $(TEST_BIN) *.gcda *.gcno
	@echo "clean"

run: $(BIN)
	./$(BIN) -m 256 -k vmlinux -d disk.img --create-disk --stats

help:
	@echo "targets: all test lint format dtb coverage clean run lib"
	@echo "variables: MODE=release|debug|sanity  WERROR=1  TEST_ARGS=-v"

# Dependency tracking
$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

-include $(DEP)
