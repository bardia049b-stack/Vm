# Architecture

RVM is a system-level RISC-V emulator: it models a whole board, not just an
instruction set. Everything below exists to let an unmodified Debian kernel and
userland run on it, on a desktop and on Android, in well under 5 MB of binary.

```
   +--------------------------------------------------------------+
   |  main.c / ui/            CLI, terminal handling              |
   +--------------------------------------------------------------+
   |  vm/vm.c                 machine assembly + run loop         |
   |  vm/sbi.c                SBI v2.0 (timer, console, IPI, HSM) |
   +--------------------------------------------------------------+
   |  cpu/       mmu/         bus/                                |
   |  core+CSR   Sv39/48/57   RAM + MMIO dispatch                 |
   +--------------------------------------------------------------+
   |  devices/  uart  clint  plic  virtio-mmio  virtio-blk        |
   +--------------------------------------------------------------+
   |  loader/   ELF64 loader, raw blob loader, FDT builder        |
   +--------------------------------------------------------------+
                      libc + libm only
```

Every arrow points down. `cpu/` has never heard of `virtio`; it reaches memory
through `bus_load`/`bus_store` function pointers and traps otherwise. That is
what makes the test suite able to drive the CPU with a hand-built bus and no
VM at all — see `tests/fixtures/harness.c`.

## The memory map

One hart, one address space, matching what `dtb/rvm.dts` and
`src/loader/fdt.c` both describe:

| Region | Address | Size | Notes |
| --- | --- | --- | --- |
| CLINT | `0x0200_0000` | 64 KiB | `msip`, `mtimecmp`, `mtime`; 10 MHz timebase |
| PLIC | `0x0c00_0000` | 64 MiB | 31 sources, one M and one S context |
| UART0 | `0x1000_0000` | 256 B | ns16550a, IRQ 10 |
| virtio-mmio | `0x1000_1000` | 8 × 4 KiB | slots 0–7, IRQs 1–8 |
| RAM | `0x8000_0000` | 8 MiB – 8 GiB | default 1 GiB |

`src/rvm.h` is the single source of truth for these numbers; the FDT builder and
the `.dts` are both derived from it and `test_fdt_blob` checks the result.

## CPU

`cpu_exec32()` in `src/cpu/cpu.c` is one `switch (OP(insn))` over the whole
RV64GC space. Three things are worth calling out:

**Compressed instructions are normalised, not duplicated.** `c_expand()`
(`src/cpu/compressed.c`) turns any 16-bit instruction into its exact 32-bit
equivalent and hands that to the same execute path. There is no second copy of
the arithmetic, so `c.addw` cannot drift away from `addw`. The decompressor
emits real encodings rather than an IR, which keeps it auditable: you can
`--trace` and see the expanded instruction.

**Every non-branching path falls through to one epilogue.** The last line of
`cpu_exec32` is

```c
c->pc = taken ? npc : (c->pc + (c->last_insn_len ? c->last_insn_len : 4));
```

Any case that returns early — the AMO helper, the FP helper, `mret`/`sret` —
must advance `pc` itself or re-arm `npc`. Three separate bugs in the first
working version were exactly this, and all three showed up as an instruction
executing forever. The rule is worth remembering before adding an opcode.

**Corner cases are guarded before the host operation, not after.** x86 raises
`SIGFPE` on `INT64_MIN / -1` and on `INT32_MIN % -1`, so the spec's defined
results have to be selected *before* the C division. The same applies to
`MULHSU`, where `rs2` is unsigned: converting the `u64` to `__int128` preserves
its value, which is exactly the required zero-extension.

`csr.c` holds the CSR file. `mstatus.SD` is never stored — it is computed on
read from `FS`/`XS`, because it is architecturally read-only and a stored copy
goes stale the moment an FP instruction retires.

## MMU

`mmu_translate()` is one function used by fetch, load and store. It takes the
effective privilege and `mxr`/`sum` as arguments rather than reaching into the
CPU, so the test suite can drive Sv48 walks directly.

- Sv39/48/57 share one loop parameterised by `levels`.
- Superpages are supported, including the misaligned-PPN check that rejects a
  megapage whose `PPN[0]` is non-zero.
- `A`/`D` are managed by hardware, which the spec explicitly permits and which
  avoids the trap storm a software-managed implementation causes on Linux.
- A direct-mapped TLB keyed on the page number, with a generation tag so
  `sfence.vma` can invalidate by bumping the tag instead of walking 256
  entries.

Two permission rules that are easy to get wrong and are covered by tests:
`R=0,W=1` is reserved and faults; **`W=1,X=1` is legal**. Treating the second
as reserved faults on every writable-and-executable page Linux maps.

## Devices

Each device is a `(load, store)` pair over a `u64` address and size, attached
to the bus with `bus_attach()`. Nothing in `bus.c` knows what a UART is.

- **UART** — ns16550a with a real 16-byte FIFO, DLAB divisor registers and an
  interrupt line. `uart_set_sink()` redirects transmitted bytes; the CLI sends
  them to stdout, the test harness sends them to a buffer it can assert on.
- **CLINT** — `mtime` advances from a host clock at a 10 MHz guest timebase.
  `mtimecmp` comparison produces `MIP_MTIP`.
- **PLIC** — priority, per-context enable bits, pending bits, and the
  claim/complete handshake with the usual threshold register.
- **virtio-mmio** — the transport lives in `virtio.c`; device types plug in
  behind it. `virtio_blk.c` implements the split-virtqueue protocol against a
  host file with `pread`/`pwrite`, so a guest `write()` really does reach the
  disk and `test_virtio_blk_roundtrip` can read the file back with `pread` to
  prove it.

Devices raise interrupts by calling a `raise(ud, irq, level)` callback, which
the VM wires to `plic_raise()`. The direction is the same as the memory one:
devices point down at the bus, never up at the CPU.

## Loader and device tree

`loader_load_elf()` parses ELF64 program headers and copies `PT_LOAD` segments
to their physical addresses, rejecting anything outside RAM. `loader_load_blob()`
handles raw `Image` files at a given base.

`fdt.c` builds the DTB in C. That is deliberate: `dtc` is not installed
everywhere (and not on Android), and a VM that cannot describe its own machine
cannot boot. `dtb/rvm.dts` is the same machine written by hand for editing;
`test_fdt_blob` walks the generated blob and checks that the nodes Linux needs
are present with the right `reg` cell order.

## VM and run loop

`vm.c` assembles the machine and owns the loop:

```
while (!shutdown) {
    hw_time = clint_tick();          /* advances mtime, raises MTIP */
    hw_mip  = plic_update();         /* highest pending, gated by threshold */
    switch (cpu_step()) {
        STEP_OK:       continue;
        STEP_TRAP:     continue;     /* the trap already redirected pc */
        STEP_ECALL_M:  sbi_call();   /* SBI is handled here, not in the CPU */
        STEP_WFI:      idle();       /* advance time without burning host CPU */
        STEP_FAULT:    report();
        STEP_SHUTDOWN: return;
    }
}
```

The CPU has no idea SBI exists. It traps on `ecall` from M-mode and the run
loop interprets it. That keeps `cpu.c` free of policy and lets `sbi.c` be
tested on its own.

## What is deliberately not here

- **No JIT for cold code.** Blocks are interpreted; only blocks that have run
  50+ times are cached (PLAN.md step 12). The cache is keyed on guest PC and
  falls back to the interpreter on any miss, so correctness never depends on it.
- **No DMA.** virtio transfers are `memcpy` between the guest buffer and the
  host. A scatter-gather engine would be faster and much harder to reason about.
- **No threads in the VM.** One hart, one thread, no locks. The UI polls; it
  does not share the CPU struct.
- **No heavy dependencies.** libc and libm. On Android that is what keeps the
  APK under 5 MB: no AndroidX, no Material, no Compose, no WebView. The
  terminal is a `Canvas`, the framebuffer is a `SurfaceView`.
