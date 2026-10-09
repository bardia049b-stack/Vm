# Changelog

All notable changes to RVM are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project
adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Planned

See [PLAN.md](PLAN.md) steps 9–12: virtio-net, virtio-gpu and virtio-input,
virtio-snd, and the hot-block JIT.

## [0.1.0] — not yet tagged

Tagging `v0.1.0` is what fires `.github/workflows/release.yml`. Everything
below is on `main` and green in CI, but no release has been cut yet.

The first version that boots a bare-metal guest, passes its own test suite and
builds an installable APK in CI.

### Added

**Core.** An RV64GC emulator in C11 with no dependencies beyond libc and libm.

- `src/cpu/` — RV64I plus M, A, F, D and C. Compressed instructions are
  expanded to their 32-bit form before decode, so there is one execution path.
  `src/cpu/fp.c` implements the FP unit including `fflags` accumulation, `frm`
  rounding and `fcvt` saturation.
- `src/cpu/csr.c` — machine and supervisor CSRs, the full trap path with
  `medeleg`/`mideleg` delegation, `mret`/`sret`, `wfi`, `sfence.vma`, `ecall`
  and `ebreak`.
- `src/mmu/` — Sv39, Sv48 and Sv57, superpages at every level, the complete
  permission matrix including W-without-X, SUM and MXR, and a direct-mapped TLB
  invalidated by `sfence.vma` with asid and vaddr honoured.
- `src/bus/` — a flat sorted region table with load/store entry points; devices
  attach by base and size.
- `src/vm/sbi.c` — the minimal SBI a real kernel needs: base, timer, console
  putchar, system reset.
- `src/devices/` — ns16550a UART at `0x10000000` (IRQ 10) with a FIFO and a
  test-visible sink; CLINT at `0x02000000` driven by `clock_gettime`; PLIC at
  `0x0C000000` with priorities, enables, threshold, claim and complete; the
  virtio-mmio transport and split virtqueue, with eight slots at `0x10001000`
  and IRQs 1–8; and virtio-blk on slot 0.
- `src/loader/` — ELF64 and flat-binary loading, plus an in-process FDT builder
  that emits the same device tree as the checked-in `dtb/rvm.dts`.
- `src/ui/` — raw-mode terminal handling for the desktop front-end.
- `src/main.c` — the `rvm` command line, including `--dump-dtb`, `--create-disk`,
  `--stats` and `--trace`.

**Debugging.**

- `RVM_TRACE_FROM` / `--trace-from HEX` — arms the instruction trace at one
  guest PC and drops everything before it, so catching a guest that jumps into
  data does not require a multi-gigabyte log.

**Tests.** 31 suites, 583 assertions, in `tests/`, driven by a small harness
(`tests/fixtures/`) that builds a real bus, MMU, CPU, CLINT, PLIC and UART and
emits guest instructions into it. Covers the ALU, immediates, shifts, loads and
stores, branches, jumps, LUI/AUIPC, word ops, mul/div, AMO, compressed
instructions, CSRs, trap/mret, floating point, illegal-instruction detection,
the trace gate, Bare/Sv39/Sv48 MMU modes, page permissions, the TLB,
identity-mapped execution, the UART, CLINT, PLIC, the virtio transport, a
virtio-blk round trip in both directions, ELF and blob loading, and FDT
generation.

**Android.** `android/` — a single `android.app.Activity` with three custom
views and no third-party dependencies at all:

- `RvmView` draws the console with `Canvas` in a monospace face; there is no
  WebView anywhere.
- `GfxView` is a `SurfaceView` ready for the virtio-gpu framebuffer.
- `KeyBar` supplies the small-but-touchable Ctrl, Esc, Tab and arrow keys.
- `RvmNative` is the whole JNI surface. The VM runs on its own thread; input
  reaches it through a lock-free SPSC ring and `vm.running` is `_Atomic`, which
  is the only shared state between the two threads.
- `android/jni/` compiles `src/` directly through CMake, so there is one copy
  of the emulator and the desktop build cannot drift from the phone build.
  `ANDROID_STL=none`; arm64-v8a and x86_64 only.
- The release APK is **65 KB** against a 5 MB budget.

**Build and CI.**

- A plain GNU `Makefile` with `MODE=release|debug|sanity`, `WERROR=1` and
  `TEST_ARGS`, plus `all test lint format dtb coverage clean run lib help`.
- `scripts/build.sh`, `scripts/test.sh`, `scripts/clean.sh`.
- `tools/mkrootfs.sh` (debootstrap → `disk.img`), `tools/fetch_kernel.sh`
  (Debian pool → `vmlinux`), `tools/run.sh` (the whole boot sequence).
- Five GitHub Actions workflows: `build` (gcc-13, gcc-14, clang-18 on Linux and
  Apple clang on macOS, with ccache and artefacts), `test` (two compilers, an
  ASan+UBSan job and an lcov coverage job), `lint` (clang-format pinned to the
  LLVM 15.0.0 that formatted the tree, `-Werror` builds and shellcheck),
  `android` (debug and release APKs with a hard 5 MB gate) and `release` (on
  `v*` tags: gate, build all three platforms, publish to a GitHub Release with
  generated notes and a sha256 table).

### Fixed

Recorded here because each one is a trap that will be walked into again.

- Signed overflow in `addw`, `subw`, `addiw` and `mul`. RISC-V specifies a
  wrap; C specifies undefined behaviour. These now compute in the matching
  unsigned type. Found by the sanitizers job.
- `sext()` and `imm_clui()` shifted negative signed values left, which is UB.
  The shift is now performed on the unsigned value. Found by UBSan.
- The ELF builder's `PUT16`/`PUT32`/`PUT64` macros shifted the caller's
  expression, so `PUT64(40, 0)` shifted a 32-bit `int` by 32. Found by UBSan.
- `make coverage` had `clean` as a prerequisite, which under `-j` deletes
  `build/` while parallel compiles are writing into it. It now runs two
  sequential sub-makes.
- `scripts/*.sh` only accepted `-j 4`, so CI's `-j"$(nproc)"` was rejected as an
  unknown option. `-jN` and `--jobs=N` work now.
- `dtb/rvm.dts` referenced `&plic` on a node that carried only a hand-written
  `phandle` property, which `dtc` rejects. Interrupt controllers have real
  labels and `dtc` allocates the phandles.
- `android/settings.gradle.kts` declared plugin repositories but no project
  repositories, so AGP could not resolve its own `aapt2`.
- `android-actions/setup-android@v3` installs the retired `tools` package by
  default and fails; both Android workflows now pass an explicit package list.

[Unreleased]: https://github.com/bardia049b-stack/Vm/commits/main
[0.1.0]: https://github.com/bardia049b-stack/Vm/commits/main
