# PLAN

The build order for RVM. Twelve steps, each one ending in something that runs
and something that is tested — no step is allowed to leave the tree red.

This file is the authoritative list. [README.md](README.md) summarises it,
[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) explains the shape of the result,
and [CHANGELOG.md](CHANGELOG.md) records what actually landed.

**Status legend** — ✅ done and tested · 🟡 code complete, not yet verified on
real hardware/images · ⬜ not started

| # | Step | Status |
|---|------|--------|
| 1 | RV64I core | ✅ |
| 2 | UART, "Hello, RVM!" | ✅ |
| 3 | CSRs and traps | ✅ |
| 4 | MMU: Sv39, Sv48, Sv57 | ✅ |
| 5 | CLINT | ✅ |
| 6 | PLIC | ✅ |
| 7 | virtio-blk end to end | ✅ |
| 8 | Boot Debian | 🟡 |
| 9 | Networking (virtio-net) | ⬜ |
| 10 | Graphics (virtio-gpu, virtio-input) | ⬜ |
| 11 | Audio (virtio-snd) | ⬜ |
| 12 | Hot-block JIT | ⬜ |

---

## Ground rules

These are constraints, not preferences. Every step below is designed around
them, and a change that breaks one needs an explicit decision recorded here.

**Lightweight.** C11, libc and libm only. No glib, no zlib, no third-party
anything. The desktop binary is under 300 KB stripped of symbols; the Android
APK must stay under 5 MB and CI fails the build if it does not.

**No AndroidX, no Material, no Compose, no WebView.** The Android UI is one
`android.app.Activity`, three custom `View`s and a `LinearLayout` of buttons.
The terminal is drawn with `Canvas`; graphics go to a `SurfaceView`; audio is
plain AAudio. `android/app/build.gradle.kts` has an empty `dependencies {}`
block and it should stay that way.

**Single-threaded.** The VM owns one thread. No locks anywhere in `src/`, and
no allocations on the hot path — memory, the TLB and the virtio rings are all
allocated once at `vm_new()`. The only concurrency in the whole project is the
Android JNI shim, which crosses exactly one boundary: a lock-free SPSC ring
from the UI thread into the VM thread, and `_Atomic bool vm.running` for stop.

**No complex DMA.** Device transfers are `memcpy` between the guest's
descriptor chain and a host buffer. No scatter-gather engines, no IOMMU, no
async queues.

**No complex JIT.** When step 12 lands it translates hot basic blocks only,
keys the cache on the guest PC, falls back to the interpreter for everything
else, and does no register allocation, no inlining and no optimisation passes
beyond what falls out naturally.

**Soft colours, no decoration.** `#F5F4F0` paper, `#2B2B28` ink, `#D8D5CE`
rules. Monospace in the terminal. No animation, no elevation, no gradients, no
shadows. Icons are single-weight line vectors.

---

## Step 1 — RV64I core ✅

`src/cpu/cpu.c`, `src/cpu/compressed.c`.

RV64I, then the M, A, F, D and C extensions on top: `src/cpu/fp.c` holds the
floating-point unit and its `fflags`/`frm` handling, and `compressed.c`
expands every 16-bit form into its 32-bit equivalent before decode so there is
exactly one execution path.

Acceptance: `tests/test_cpu.c` — ALU, immediates, shifts, loads and stores,
branches, jumps, LUI/AUIPC, the W-form word ops, mul/div, AMO, compressed,
CSRs, trap/mret, FP and illegal-instruction detection.

Things that were wrong and are now guarded by a test, because they are the
traps everyone falls into:

- LUI sign-extends its immediate; `th_li()` in the harness zero-extends to 32
  bits, so tests that build addresses must not assume otherwise.
- The B- and J-type immediates are bit-permuted, not contiguous. An off-by-one
  in the reassembly silently corrupts every branch in the system.
- `funct6`, not `funct7`, discriminates the RV64 shift-immediates.
- `INT64_MIN / -1` and `INT64_MIN % -1` must be handled before the host
  division runs, or the host raises SIGFPE.
- `mulhsu` zero-extends rs2.
- `amomin`/`amomax` compare signed; `amominu`/`amomaxu` compare zero-extended.
  The value written back to the register is always the *old* memory value.
- `fcvt` to integer saturates out of range (signed → `INT_MAX`/`INT_MIN`,
  unsigned → `0`/`UINT_MAX`) and raises only NV; NaN uses the canonical
  invalid pattern. Earlier inexactness still accumulates in `fflags`.
- Anything that returns from `cpu_exec32()` before the shared epilogue must
  advance `c->pc` itself, and anything that writes `c->pc` (`mret`, `sret`)
  must have its result copied into `npc`.
- Signed overflow is undefined in C even where RISC-V specifies a wrap. `addw`,
  `subw`, `addiw` and `mul` are all computed in the matching unsigned type.

## Step 2 — UART ✅

`src/devices/uart.c`. An ns16550a at `0x10000000`, IRQ 10, with a small FIFO
and a writable sink so tests can capture output instead of printing it.

Acceptance: a bare-metal guest prints `Hello, RVM!`; `test_uart_hello` and
`test_uart_fifo_irq` cover the register file, the FIFO and the interrupt line.

## Step 3 — CSRs and traps ✅

`src/cpu/csr.c`, and the `SYSTEM` opcode in `cpu.c`.

Machine and supervisor CSRs, the full trap path (cause, `*epc`, `*tval`,
`*status` stacking, delegation through `medeleg`/`mideleg`), `mret`/`sret`,
`wfi`, `sfence.vma`, `ecall` and `ebreak`. `src/vm/sbi.c` provides the minimal
SBI a real kernel needs: base, timer, console putchar and system reset.

Acceptance: `test_cpu_csr`, `test_cpu_trap_mret`, `test_cpu_illegal`.

## Step 4 — MMU ✅

`src/mmu/mmu.c`. Sv39, Sv48 and Sv57 page tables, superpages at every level,
the full permission matrix including the W-without-X fault, SUM/MXR, and a
direct-mapped TLB invalidated by `sfence.vma` (with asid/vaddr honoured).

Acceptance: `test_mmu_bare`, `test_mmu_sv39`, `test_mmu_sv48`,
`test_mmu_permissions`, `test_mmu_tlb`, `test_mmu_identity_exec`.

## Step 5 — CLINT ✅

`src/devices/clint.c` at `0x02000000`: `msip`, `mtimecmp` per hart and a
monotonic `mtime` driven by `clock_gettime`, so the guest sees time pass at the
host's rate rather than at a made-up one.

Acceptance: `test_clint_mtimecmp`.

## Step 6 — PLIC ✅

`src/devices/plic.c` at `0x0C000000`: priorities, enables, threshold, claim and
complete over the external interrupt lines of every attached device.

Acceptance: `test_plic_claim`.

## Step 7 — virtio-blk ✅

`src/devices/virtio.c` (the mmio transport and split virtqueue) and
`src/devices/virtio_blk.c` (the device). Legacy-free virtio 2, eight mmio slots
at `0x10001000` with a `0x1000` stride, IRQs 1..8. Empty slots decode as
"no device present" so the guest's probe does not fault.

Acceptance: `test_virtio_transport` and `test_virtio_blk_roundtrip` drive a
real descriptor chain from the guest side — header, data, status — in both
directions, and check the used-ring length accounting.

Transport details worth remembering when writing the next device:

- The request header is 16 readable bytes `{type, reserved, sector}`.
- Data is readable for `T_OUT` and carries `VRING_WRITE` for `T_IN`.
- The last descriptor is one writable status byte.
- The used-ring `len` counts *all* device-written bytes, status included.
- Reusing a descriptor chain across a direction change needs `put_desc` to be
  re-issued with the flipped `VRING_WRITE` flag.

## Step 8 — Boot Debian 🟡

**Code complete, and deliberately not claimed as verified.** Building a Debian
rootfs needs root, a couple of gigabytes of RAM and network access to
`deb.debian.org`; none of that exists in the environment where this was
written. What ships is the tooling, tested as far as it can be without those
prerequisites, plus the in-process FDT builder that step 8 actually depends on
and that *is* unit-tested (`test_fdt_blob`, `test_loader_elf`).

`tools/mkrootfs.sh` — `debootstrap --arch=riscv64 --variant=minbase trixie`,
run through `qemu-user-static` chroot with `--second-stage`, `sysvinit-core`
instead of systemd, a root password, an `inittab` with a `ttyS0` getty and an
`fstab` for `/dev/vda`, `/proc` and `/sys`. Then `mke2fs -d rootfs` into a
`dd`-sized `disk.img`.

`tools/fetch_kernel.sh` — pulls `linux-image-6.12+deb13-riscv64` from the
Debian pool and extracts `vmlinux`.

`dtb/rvm.dts` — `compatible = "rvm,virt"`, `memory@80000000`, `cpus` with
`riscv,isa = "rv64imafdc"` and `mmu-type = "riscv,sv57"`, `serial@10000000`
(ns16550a, IRQ 10), `clint@2000000`, `plic@c000000` and the eight
`virtio_mmio` nodes. `make dtb` runs `dtc -I dts -O dtb`. The emulator builds
the same tree in C at boot (`src/loader/fdt.c`), so the checked-in `.dts` and
the generated DTB cannot drift: `--dump-dtb` writes the generated one and
`dtc -I dtb -O dts` on it should diff clean against this file.

`tools/run.sh` — the whole sequence, then `rvm -k vmlinux -d disk.img`.

Acceptance, still to be met on a machine that can build the image:

```
./tools/mkrootfs.sh --size 2048
./tools/fetch_kernel.sh
./tools/run.sh
...
Debian GNU/Linux 13 rvm ttyS0

rvm login:
```

Two `.dts` rules learned the hard way: give interrupt controllers real labels
(`plic: plic@c000000`) rather than hand-written `phandle` properties, because
`dtc` rejects a `&plic` reference to a node it cannot label; and never write
`phandle = <N>` by hand at all — let `dtc` allocate them.

## Step 9 — Networking ⬜

`src/devices/virtio_net.c` on virtio slot 1. Receive and transmit queues, a
TAP backend (`rvm0`) on Linux, and — because a phone has no TAP device — a
user-mode fallback that terminates the link layer in the emulator.

The user-mode stack stays minimal on purpose: ARP replies for the gateway, a
DHCP offer of `10.0.2.15/24` with gateway `10.0.2.2`, enough IP to route, and
just enough TCP and UDP to carry a connection through NAT. No reassembly of
fragmented datagrams, no IPv6, no socket options beyond the ones a browser or
`apt` actually uses.

Host side, for the TAP path:

```sh
ip tuntap add dev rvm0 mode tap
ip link set rvm0 up
ip addr add 10.0.2.1/24 dev rvm0
echo 1 > /proc/sys/net/ipv4/ip_forward
iptables -t nat -A POSTROUTING -s 10.0.2.0/24 -j MASQUERADE
```

Acceptance: inside the guest, `apt update`, `curl`, `wget`, `ssh` out and
`ping` all work. `tests/test_net.c` covers ARP, the DHCP offer, one TCP
handshake and one UDP round trip against a loopback backend, with no host
network access required.

## Step 10 — Graphics ⬜

`src/devices/virtio_gpu.c` on slot 2 and `src/devices/virtio_input.c` on slot
3. The GPU implements only what a display driver needs: `resource_create_2d`,
`resource_attach_backing`, `transfer_to_host_2d`, `set_scanout` and
`resource_flush`, into a host framebuffer. Input reports `EV_REL` and `EV_KEY`
from the Android touch layer.

`GfxView` (the existing `SurfaceView`) receives the flushed framebuffer through
the `onFrame(byte[] rgba, int w, int h, int stride)` JNI callback that is
already declared, kept by ProGuard and wired in `android/jni/rvm_jni.c`.

Acceptance: X11 or Wayland comes up inside the guest, a window is visible on
the Android surface, and the pointer moves.

## Step 11 — Audio ⬜

`src/devices/virtio_snd.c` on slot 4. PCM only: one queue, one stream, no
mixer, no volume control, no jack enumeration beyond a single output. The host
end writes to AAudio on Android and to `/dev/dsp`-style stdout or a null sink
elsewhere. `android/jni/CMakeLists.txt` already links `aaudio` when the NDK
provides it and defines `RVM_HAVE_AAUDIO`.

Acceptance: `aplay` plays a WAV inside the guest and it is audible on the
phone.

## Step 12 — Hot-block JIT ⬜

`src/cpu/jit.c`. Count executions per basic block; after 50, translate the
block into host code and cache it keyed on the guest PC. Everything else keeps
running in the interpreter, which stays the reference implementation and the
fallback for any block the translator declines.

The cache must be flushed on `sfence.vma` and on any write to a page that has
been translated, or self-modifying code — which Linux does use, in modules and
in `ftrace` — silently executes stale host instructions.

No register allocation beyond mapping the guest register file to a fixed host
location, no inlining across blocks, no optimisation passes.

Acceptance: 500+ MIPS on a sustained integer loop, measured by `--stats`, with
`tests/test_jit.c` proving the JIT and the interpreter agree instruction for
instruction on the whole existing suite.

---

## The bug that gets its own line

`pc = 0xfffffffffffffffe`.

The guest jumps to data. It shows up as a PC that is `-2`, which is what you
get when a `jalr` or a corrupted return address lands two bytes below zero, and
it always means the emulator executed something the guest never intended as
code — usually a decode bug that retired an instruction with the wrong length,
or a trap path that wrote `*epc` from a stale `npc`.

The recipe is in [docs/DEBUGGING.md](docs/DEBUGGING.md): narrow the window with
`-n`, arm the trace gate with `RVM_TRACE_FROM=<pc>` just before the last known
good address, read the final retired instruction, then resolve it with
`objdump -d vmlinux` and `addr2line -e vmlinux -f -C <pc>`. Tracing the whole
boot produces gigabytes and answers nothing; the gate is what makes this
tractable.

Every one of the decode and trap bugs listed under step 1 was found this way.
