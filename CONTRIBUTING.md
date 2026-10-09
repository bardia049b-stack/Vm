# Contributing

RVM is a small emulator with a hard size budget and a test suite that is
expected to stay green. Most of this document is about not breaking either.

## Before you start

Read [PLAN.md](PLAN.md) for what is being built next and the constraints every
step has to satisfy, and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for how
the pieces fit together. If your change contradicts a ground rule in PLAN.md —
adding a dependency, introducing a thread, pulling in AndroidX — say so in the
issue first rather than in the pull request.

## Getting a build

```sh
git clone https://github.com/bardia049b-stack/Vm.git
cd Vm
make -j"$(nproc)"          # builds ./rvm
make test                  # builds and runs the suite
./rvm --help
```

Full instructions, including macOS and the Android APK, are in
[docs/BUILDING.md](docs/BUILDING.md).

Requirements: a C11 compiler and GNU Make. That is all. `dtc` is needed only
for `make dtb`, `lcov` only for `make coverage`, and the Android SDK plus NDK
only for the APK.

## The loop

```sh
make lint                  # -Werror syntax pass over src/ and tests/
make test TEST_ARGS=-v     # per-suite progress
make MODE=sanity test      # ASan + UBSan
make format                # clang-format -i (needs clang-format 15)
./scripts/test.sh --werror # optimised build with -Werror
```

`make lint` is `-fsyntax-only`, so it does not see warnings that only appear
once the compiler folds constants at `-O2`. `./scripts/test.sh --werror` (or
`WERROR=1` in the environment) builds the suite optimised with `-Werror` and
does catch them. CI runs both.

## Style

**C.** `.clang-format` is the whole policy: LLVM base, 4 spaces, 100 columns,
attached braces, right-aligned pointers. Run `make format` before you commit;
CI pins clang-format to LLVM 15.0.0 because that is the version that formatted
the tree, and a different major version will produce diffs.

`-Wall -Wextra -Wshadow -Wundef -Wpointer-arith -Wcast-qual
-Wstrict-prototypes -Wmissing-prototypes` are on for every build, so every
non-static function needs a prototype in a header, and unused static helpers
are an error under `WERROR=1`.

**Shell.** `shellcheck -x tools/*.sh scripts/*.sh` must be clean. Note that
shellcheck exits non-zero on *info*-level findings too: use `find` rather than
`ls` for globbing, quote expansions inside `${VAR#"$prefix"}`, and write
`{ [ a ] || [ b ]; }` instead of the ill-defined `[ a -o b ]`.

**Kotlin/Groovy.** The Gradle files are `.kts` and stay that way.

**Commits.** Imperative subject line, no period. Explain *why* in the body —
the diff already shows what. A commit that fixes a bug should say what the
symptom was and how it was found.

## Adding a test

Suites are declared once, in `tests/test_suites.h`, and registered with one
`RUN()` line in `tests/test_main.c`. Nothing else needs to change; the Makefile
globs `tests/*.c`.

```c
void test_my_thing(void);                    /* tests/test_suites.h */
RUN(test_my_thing);                          /* tests/test_main.c   */
```

The harness in `tests/fixtures/` builds a real bus, MMU, CPU, CLINT, PLIC and
UART. Emit guest code with `th_emit()` (or `th_emit16()` for compressed forms),
set up registers, then `th_run_all()` — it runs until the PC reaches the end of
the emitted stream, so helper instructions from `th_li()` cannot throw off an
explicit step count. Use `QUIET_BEGIN()`/`QUIET_END()` around anything that
would otherwise print.

Three traps that have already cost time:

- `th_li()` **zero-extends to 32 bits**. Use it for addresses, but check the
  expectation you write against that.
- `th_run_all()` does **not** stop on `STEP_TRAP`. An all-zero register file
  after a block means the guest trapped and looped, not that the arithmetic was
  wrong.
- Derive expected values from the spec text, not from memory of the ISA.

## Adding a device

1. `src/devices/foo.c` and `foo.h`, following `virtio_blk.c` for the shape.
2. Attach it to a virtio slot in `vm_new()` and give it an IRQ via
   `RVM_VIRTIO_IRQ(n)`.
3. Add the node to **both** `dtb/rvm.dts` and `src/loader/fdt.c`. They must
   stay identical: `./rvm --dump-dtb /tmp/a.dtb && dtc -I dtb -O dts /tmp/a.dtb`
   should match `dtc -I dts -O dtb dtb/rvm.dts && dtc -I dtb -O dts dtb/rvm.dtb`.
4. Add the source to nothing else — `android/jni/CMakeLists.txt` globs
   `src/*.c` with `CONFIGURE_DEPENDS`.
5. Test the transport behaviour in `tests/`, not just the device logic.

## Things that will get a change sent back

- A new dependency. Any library, any header-only thing, any vendored `.c` from
  another project.
- A lock in `src/`. The VM is single-threaded; the only shared state in the
  project is the JNI input ring and `vm.running`.
- An allocation on the hot path. Memory, the TLB and the virtio rings are
  allocated once in `vm_new()`.
- AndroidX, Material Components, Compose or a WebView.
- An APK over 5 MB. CI fails the build, not just the check.
- A commit that reformats code you did not otherwise touch.
- `phandle = <N>` written by hand in a `.dts`. Let `dtc` allocate them.

## Pull requests

Keep them to one idea. A PR that fixes a bug and refactors the file it lives in
is two PRs.

The description should say what the change does, why the previous behaviour was
wrong, and how you verified it. If it fixes a decode or trap bug, include the
trace or the objdump line that proved it — see
[docs/DEBUGGING.md](docs/DEBUGGING.md).

CI must be green: `build` (gcc-13, gcc-14, clang-18, Apple clang), `test`
(two compilers, sanitizers, coverage), `lint` (format, warnings, shellcheck)
and `android` (debug and release APK). Badges are at the top of
[README.md](README.md).

## Security

Guest escapes are security bugs, not correctness bugs. See
[SECURITY.md](SECURITY.md) for how to report one — please do not open a public
issue for it.
