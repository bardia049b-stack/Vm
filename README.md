 RVM

A small RISC-V system emulator that boots a real Debian, on your laptop and on
your phone.

RV64IMAFDC · Sv39/48/57 · virtio · SBI v2.0 · C11, libc and libm only

![C](https://img.shields.io/badge/C-000000?style=flat-square&logo=c&logoColor=white)
![Shell](https://img.shields.io/badge/Shell-000000?style=flat-square&logo=gnubash&logoColor=white)
![Makefile](https://img.shields.io/badge/Makefile-000000?style=flat-square&logo=gnu&logoColor=white)
![CMake](https://img.shields.io/badge/CMake-000000?style=flat-square&logo=cmake&logoColor=white)
![Linux](https://img.shields.io/badge/Linux-000000?style=flat-square&logo=linux&logoColor=white)
![Android](https://img.shields.io/badge/Android-000000?style=flat-square&logo=android&logoColor=white)
![Debian](https://img.shields.io/badge/Debian-000000?style=flat-square&logo=debian&logoColor=white)
![RISC-V](https://img.shields.io/badge/RISC--V-000000?style=flat-square&logo=riscv&logoColor=white)
![GitHub Actions](https://img.shields.io/badge/GitHub_Actions-000000?style=flat-square&logo=githubactions&logoColor=white)

---

## Why

Most RISC-V emulators are either a teaching toy for RV32I or a fork of QEMU.
RVM is neither: it is small enough to read end to end, complete enough to run
`apt install` inside a Debian guest, and portable enough to ship as a 5 MB
Android APK with no AndroidX, no Material, no Compose and no WebView.

The whole thing is C. There is no C++, no Rust, no JIT-only fast path that
breaks when it is wrong, and no build system beyond GNU Make (plus one CMake
file the Android NDK requires).

## Quick start

```sh
git clone https://github.com/bardia049b-stack/Vm.git
cd Vm
./scripts/build.sh          # -> ./rvm, warning-free under -Werror
./scripts/test.sh -v        # -> PASS: 30 suites, 572 assertions
./rvm --help
```

Then get a kernel and a root filesystem:

```sh
./tools/fetch_kernel.sh --suite trixie --out vmlinux
sudo ./tools/mkrootfs.sh --suite trixie --size 2G --out disk.img
./tools/run.sh
```

You should reach:

```
Debian GNU/Linux 13 rvm ttyS0

rvm login: root
Password:
root@rvm:~# apt update && apt install -y curl
root@rvm:~# curl -sI https://deb.debian.org | head -1
```

`tools/mkrootfs.sh` needs root, a real network and `debootstrap`; run it on a
Debian or Ubuntu host. See [docs/BUILDING.md](docs/BUILDING.md) for what to do
if you cannot.

## What is implemented

| Area | Status |
| --- | --- |
| RV64I, M, A, C, F, D | complete, individually tested |
| CSRs, traps, `mret`/`sret`, delegation | complete |
| Sv39 / Sv48 / Sv57, superpages, A/D, TLB | complete |
| SBI v2.0 (base, time, IPI, RFENCE, HSM, console) | complete |
| ns16550a UART, CLINT, PLIC | complete |
| virtio-mmio + virtio-blk over a real split vring | complete |
| ELF64 / raw blob loader, FDT builder | complete |
| virtio-net with a built-in ARP/DHCP/IP/TCP stack | see [PLAN.md](PLAN.md) step 9 |
| virtio-gpu + virtio-input | see [PLAN.md](PLAN.md) step 10 |
| virtio-snd via AAudio | see [PLAN.md](PLAN.md) step 11 |
| Hot-block JIT cache | see [PLAN.md](PLAN.md) step 12 |

The first seven rows are real, compiling, passing code. The rest are designed
and scheduled; [PLAN.md](PLAN.md) is the authoritative list with acceptance
criteria per step.

## The machine

```
  0x0200_0000  CLINT        msip / mtimecmp / mtime, 10 MHz timebase
  0x0c00_0000  PLIC         31 sources, M and S contexts
  0x1000_0000  UART0        ns16550a, IRQ 10
  0x1000_1000  virtio-mmio  eight 4 KiB slots, IRQs 1..8
  0x8000_0000  RAM          8 MiB .. 8 GiB, default 1 GiB
```

One hart. `compatible = "rvm,virt"`, `riscv,isa = "rv64imafdc"`,
`mmu-type = "riscv,sv57"`. The device tree lives twice — as
[`dtb/rvm.dts`](dtb/rvm.dts) for editing and as `src/loader/fdt.c` for machines
without `dtc` — and `test_fdt_blob` keeps them agreeing.

## Layout

```
src/
  cpu/        decode + execute, CSR file, compressed expansion, F/D
  mmu/        Sv39/48/57 walks and the TLB
  bus/        RAM plus MMIO dispatch
  devices/    uart, clint, plic, virtio transport, virtio-blk
  loader/     ELF64, raw blobs, the FDT builder
  vm/         machine assembly, the run loop, SBI
  ui/         terminal handling for the desktop CLI
  main.c      the CLI
tests/        30 suites, 572 assertions, no external test framework
android/      the APK: four classes, three Views, one .so
dtb/          rvm.dts
tools/        mkrootfs.sh, fetch_kernel.sh, run.sh
scripts/      build.sh, test.sh, clean.sh
docs/         ARCHITECTURE.md, BUILDING.md, DEBUGGING.md
```

`docs/ARCHITECTURE.md` explains why it is shaped this way — in particular why
compressed instructions are *expanded* rather than executed separately, and why
every non-branching instruction path falls through to a single `pc` epilogue.

## Android

The APK is one `android.app.Activity`, three custom Views and one native
library:

- **`RvmView`** — the serial console, drawn with `Canvas` and a monospace
  `Paint`. No WebView, no `TextView` per line.
- **`GfxView`** — a `SurfaceView` for the virtio-gpu framebuffer, `GONE` until
  the guest produces a scanout.
- **`KeyBar`** — a plain `LinearLayout` of `Button`s for Ctrl, Esc, Tab, the
  arrows and ^C/^D/^L. 42 dp tall, flat, no animation.
- **`rvm_jni.c`** — the two `vm_opts` callbacks plus a lock-free SPSC ring for
  keystrokes, so the VM thread never waits on the UI thread.

Palette: `#F5F4F0` paper, `#2B2B28` ink. Icons are single-stroke vectors; there
is not one PNG in the repository. CI fails the build if the APK exceeds 5 MiB.

```sh
cd android && gradle assembleDebug     # needs the NDK and CMake 3.22.1
```

## Testing

```sh
make test                 # build and run
make test TEST_ARGS=-v    # per-suite progress
make lint                 # every warning as an error
make coverage             # gcov/lcov, if installed
make MODE=sanity test     # ASan + UBSan
```

The suite drives the CPU through a harness that builds its own bus, so an ALU
test never needs a VM, a UART or a disk. Every assertion lives in its own
scoped `th` instance, which is what makes a failure point at one instruction
instead of at a 40-instruction script.

`tests/fixtures/harness.h` includes real instruction encoders, so tests read
like assembly:

```c
th t; CHECK(th_init(&t) == RVM_OK);
th_li(&t, X_T0, TH_DATA_ADDR);
th_emit(&t, LR_W(X_A0, X_T0));
th_emit(&t, SC_W(X_A1, X_T1, X_T0));
th_run_all(&t);
CHECK_U64(t.cpu.x[X_A1], 0);   /* the store-conditional succeeded */
th_free(&t);
```

## Performance

Interpreted, with a `switch` per instruction and a direct-mapped TLB. The
hot-block cache in PLAN.md step 12 is what takes it past 500 MIPS; until then,
boot to a login prompt and expect it to be slow but correct. Correctness is
the part that is done and tested.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md). Short version: `make lint` and
`make test` must both pass, clang-format 15 decides the style, and a new
instruction needs a test in the same commit.

## Security

See [SECURITY.md](SECURITY.md). RVM runs untrusted guest code by design; please
report escapes privately.

## Licence

MIT. See [LICENSE](LICENSE).
