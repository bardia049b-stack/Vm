# Building RVM

## Requirements

The emulator itself needs a C11 compiler and GNU Make. Nothing else.

| Tool | Needed for | Version used in CI |
|------|-----------|--------------------|
| gcc or clang | `./rvm`, `make test` | gcc-13, gcc-14, clang-18, Apple clang |
| GNU Make | everything | 4.3 |
| `dtc` | `make dtb` only | device-tree-compiler from apt/brew |
| clang-format | `make format`, the `lint` workflow | **LLVM 15.0.0** |
| shellcheck | the `lint` workflow | 0.9+ |
| lcov | `make coverage` | any |
| JDK | the Android APK | Temurin 17 |
| Android SDK, NDK, CMake | the Android APK | platform 35, NDK 27.2.12479018, CMake 3.22.1 |
| Gradle | the Android APK | 8.11.1 |
| debootstrap, qemu-user-static, e2fsprogs, root | the Debian rootfs | Debian/Ubuntu host |

Debian or Ubuntu:

```sh
sudo apt-get install -y build-essential device-tree-compiler shellcheck lcov
```

macOS:

```sh
brew install make dtc shellcheck lcov llvm   # llvm provides clang-format 15+
```

## The desktop build

```sh
make -j"$(nproc)"          # -> ./rvm
./rvm --help
```

Or through the wrapper, which also builds the DTB if `dtc` is present and
reports the binary size:

```sh
./scripts/build.sh              # release, warnings as errors
./scripts/build.sh --debug      # -O0 -g3 -DRVM_DEBUG
./scripts/build.sh --jobs 4
```

`./rvm` is about 295 KB in release mode. It links only libc and libm.

### Build modes

`MODE` selects the optimisation and instrumentation level. Switching modes
stamps `build/.mode` and cleans automatically, because reusing objects compiled
for a different mode is silent and confusing.

| `MODE=` | Flags | Use it for |
|---------|-------|-----------|
| `release` (default) | `-O2 -g` | normal work; this is what CI ships |
| `debug` | `-O0 -g3 -DRVM_DEBUG` | gdb, `--trace`, reading disassembly that matches the source |
| `sanity` | `-O1 -g -fsanitize=address,undefined` | finding memory and undefined-behaviour bugs |

```sh
make MODE=debug -j"$(nproc)"
make MODE=sanity test TEST_ARGS=-v
```

### Other variables

| Variable | Default | Meaning |
|----------|---------|---------|
| `CC` | `gcc` | the compiler; CI uses `make CC=clang-18` |
| `WERROR` | unset | `WERROR=1` adds `-Werror` to every build, not just `lint` |
| `TEST_ARGS` | empty | passed to the test runner, e.g. `TEST_ARGS=-v` |
| `OPT` | from `MODE` | overrides the optimisation flags outright |
| `EXTRA_CFLAGS`, `EXTRA_LDFLAGS` | empty | appended last, so they can override `-O` |

Note that these are Make variables, not options: `make WERROR=1`, never
`make -DWERROR=1`. Likewise `make test TEST_ARGS=-v`, never `make test -v`.

### Targets

```
all       ./rvm                     (the default)
test      build and run the suite
lint      -Werror syntax pass over src/ and tests/
format    clang-format -i over the whole tree
dtb       dtc -I dts -O dtb dtb/rvm.dts
coverage  clean rebuild with --coverage, then run the suite
lib       build/librvm.a, the core without main.c
run       ./rvm -m 256 -k vmlinux -d disk.img --create-disk --stats
clean     remove build/, ./rvm and the test binary
help      print the target and variable list
```

`make lint` is `-fsyntax-only`, which is fast but blind to warnings that only
appear once the compiler folds constants at `-O2`. Build the suite with
`-Werror` too:

```sh
./scripts/test.sh --werror -v      # or: WERROR=1 ./scripts/test.sh
```

## Tests

```sh
make test                    # quiet summary
make test TEST_ARGS=-v       # one line per suite
./scripts/test.sh -v -j4     # the same thing, plus a second run as a gate
stdbuf -o0 -e0 ./tests/rvm_tests -v
```

The `stdbuf` matters when a test crashes: without it, a SIGFPE or SIGSEGV loses
everything still sitting in the stdout buffer, and you get an empty log instead
of the name of the suite that died.

Currently 31 suites and 583 assertions. Adding one is a declaration in
`tests/test_suites.h` and a `RUN()` line in `tests/test_main.c`; see
[../CONTRIBUTING.md](../CONTRIBUTING.md).

Coverage, if `lcov` is installed:

```sh
./scripts/test.sh --coverage
lcov --summary build/coverage.info
```

## The device tree

RVM builds its FDT in C at boot (`src/loader/fdt.c`), so it needs no external
DTB. `dtb/rvm.dts` is the same tree written out by hand, for reading and for
diffing against the generated one.

```sh
make dtb                                          # dtb/rvm.dtb
./rvm --dump-dtb /tmp/generated.dtb               # what the emulator builds
dtc -I dtb -O dts /tmp/generated.dtb > /tmp/a.dts
dtc -I dtb -O dts dtb/rvm.dtb      > /tmp/b.dts
diff /tmp/a.dts /tmp/b.dts                        # must be empty
```

Two rules for editing `rvm.dts`: give interrupt controllers real labels
(`plic: plic@c000000`) so `&plic` references resolve, and never write
`phandle = <N>` by hand — `dtc` allocates them and rejects a reference to a
node whose phandle it did not assign.

## A Debian guest

This is PLAN.md step 8. It needs root, roughly 2 GB of free RAM, and network
access to `deb.debian.org`, so it does not work inside a restricted container
and CI does not attempt it.

```sh
# 1. a root filesystem -> disk.img   (needs sudo)
sudo ./tools/mkrootfs.sh --suite trixie --size 2G --out disk.img

# 2. a kernel -> vmlinux
./tools/fetch_kernel.sh --out vmlinux

# 3. boot
./tools/run.sh
```

`mkrootfs.sh` installs debootstrap, qemu-user-static and e2fsprogs if they are
missing, runs `debootstrap --arch=riscv64 --variant=minbase trixie` under a
qemu-user-static chroot with `--second-stage`, swaps systemd for
`sysvinit-core`, sets a root password, writes an `inittab` with a `ttyS0` getty
and an `fstab` for `/dev/vda`, `/proc` and `/sys`, then packs the tree into an
ext4 image with `mke2fs -d`.

`fetch_kernel.sh` downloads `linux-image-6.12+deb13-riscv64` from the Debian
pool and extracts the uncompressed ELF `vmlinux`. RVM's loader wants a real
ELF64 with `PT_LOAD` segments — not a compressed `Image` and not an EFI zboot
stub. Pass `--version 6.12.57-1` for an exact package.

Success looks like:

```
Debian GNU/Linux 13 rvm ttyS0

rvm login:
```

Both scripts are `--help`-documented and shellcheck-clean.

## The Android APK

```sh
cd android
gradle --no-daemon assembleDebug
# -> app/build/outputs/apk/debug/app-debug.apk
```

**There is no `gradle-wrapper.jar` in the repository**, so `./gradlew` does not
work. Use a Gradle 8.11.1 on your `PATH`, or let CI do it —
`gradle/actions/setup-gradle` installs the version named in
`gradle/wrapper/gradle-wrapper.properties`. Install one locally with:

```sh
sdk install gradle 8.11.1        # sdkman
# or: https://gradle.org/release-checksums/
```

What the build needs, which `sdkmanager` can install in one go:

```sh
sdkmanager --install \
  "platform-tools" "platforms;android-35" "build-tools;35.0.0" \
  "ndk;27.2.12479018" "cmake;3.22.1"
yes | sdkmanager --licenses
```

`android-actions/setup-android@v3` needs those packages listed explicitly;
without a `packages:` input it tries to install the retired `tools` package and
fails.

Install on a device:

```sh
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

The release variant is minified and resource-shrunk, and signed with the debug
key so it is installable without setting up a keystore. Do not publish that
artifact — cut a proper signing config first.

Sizes, for reference: the debug APK is about 75 KB and the release APK about
65 KB, against a 5 MB budget that CI enforces as a hard failure.

The native library is built from `src/` directly by `android/jni/CMakeLists.txt`
(which globs `src/*.c` with `CONFIGURE_DEPENDS` and drops `src/main.c`), with
`ANDROID_STL=none` and only arm64-v8a and x86_64. There is one copy of the
emulator; the desktop build and the phone build cannot drift apart.

## Continuous integration

Five workflows in `.github/workflows/`, all caching ccache or Gradle and all
cancelled-in-progress for the same ref:

| Workflow | Trigger | What it does |
|----------|---------|--------------|
| `build.yml` | push/PR to `main` | gcc-13, gcc-14 and clang-18 on `ubuntu-24.04` plus Apple clang on `macos-14`; `make dtb`; smoke-tests `--version`, `--help`, `--dump-dtb`; uploads `rvm-linux-amd64` and `rvm-macos-arm64` |
| `test.yml` | push/PR to `main` | the suite under gcc-14 and clang-18 with `-Werror`, an ASan+UBSan job with `halt_on_error=1` and leak detection, and an lcov coverage job that writes to the step summary |
| `lint.yml` | push/PR to `main` | clang-format pinned to 15.0.0 with `--dry-run --Werror` (uploads a patch on failure), `-Werror` builds under both compilers, and `shellcheck -x` |
| `android.yml` | push/PR to `main` | debug and release APKs, a size report and a hard failure above 5 MB |
| `release.yml` | tag `v*`, or manual | `gate` (format, lint, tests) then Linux, macOS and Android builds in parallel, then a GitHub Release with generated notes, sha256 sums and the three artifacts |

Cut a release by tagging:

```sh
git tag -a v0.1.0 -m "RVM 0.1.0"
git push origin v0.1.0
```

A tag containing a hyphen (`v0.2.0-rc1`) is published as a pre-release.

## Troubleshooting

**`make: invalid option -- 'D'`** — `make -DWERROR=1` is parsed as options.
Make variables are bare: `make WERROR=1`.

**Switching `MODE` reuses stale objects** — it does not, any more: `build/.mode`
is stamped and a change triggers a clean. If you see mixed-mode link errors,
`make clean` anyway.

**clang-format produces a huge diff** — you are on a different LLVM major
version than 15. `npx clang-format@1.8.0` fetches the right one, or
`brew install llvm` and use `/opt/homebrew/opt/llvm/bin/clang-format`.

**`dtc` not found** — `make dtb` and the DTB step of `build.yml` need
`device-tree-compiler`. The emulator does not: it builds its FDT in C.

**The test binary dies with no output** — run it under
`stdbuf -o0 -e0 ./tests/rvm_tests -v`. The usual cause is a SIGFPE from a
`INT64_MIN / -1` that reached the host division.

**`./gradlew: No such file or directory`** — expected; use `gradle` (see above).

**Gradle cannot resolve `com.android.tools.build:aapt2`** — the project needs
`dependencyResolutionManagement { repositories { google(); mavenCentral() } }`
in `android/settings.gradle.kts`. It is there; if you removed it, put it back.
