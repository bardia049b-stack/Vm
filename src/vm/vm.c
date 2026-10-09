/*
 * vm.c -- machine assembly and the run loop.
 *
 * Single threaded by design: one hart, no locks, no worker threads.  The loop
 * is deliberately straight-line -- pump host input, advance the time base,
 * recompute the interrupt lines, step the hart, service M-mode traps in C --
 * so that the inner path is a handful of predictable branches.
 *
 * SPDX-License-Identifier: MIT
 */
#include "vm.h"

#include <errno.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------- defaults */

void vm_opts_default(vm_opts *o) {
    memset(o, 0, sizeof(*o));
    o->ram_size = RVM_RAM_DEFAULT;
    o->bootargs = "console=ttyS0 earlycon=sbi root=/dev/vda rootwait rw";
    o->isa = "rv64imafdc";
    o->mmu_type = "riscv,sv57";
    o->dtb_addr = RVM_DTB_DEFAULT_ADDR;
    o->initrd_addr = RVM_INITRD_DEFAULT_ADDR;
    o->disk_size = 2ULL << 30; /* 2 GiB */
    o->log_level = RVM_LOG_INFO;
}

/* ------------------------------------------------------------ IRQ wiring */

static void irq_raise(void *ud, u32 irq, bool level) {
    vm *v = (vm *)ud;
    /* Our PLIC latches on the rising edge and clears on claim, so a falling
     * edge needs no action here. */
    if (level)
        plic_raise(&v->plic, irq);
    /* Either edge can change what the guest should see in mip. */
    v->irq_dirty = true;
}

static void uart_emit(void *ud, u8 ch) {
    vm *v = (vm *)ud;
    if (!v->opts.write) {
        fputc(ch, stdout);
        if (ch == '\n')
            fflush(stdout);
        return;
    }
    /* Batch into 1 KiB chunks so the Android UI is not woken per character. */
    v->outbuf[v->outlen++] = ch;
    if (v->outlen == sizeof(v->outbuf) || ch == '\n') {
        v->opts.write(v->opts.write_ud, v->outbuf, v->outlen);
        v->outlen = 0;
    }
}

static void uart_flush(vm *v) {
    if (v->outlen && v->opts.write) {
        v->opts.write(v->opts.write_ud, v->outbuf, v->outlen);
    }
    v->outlen = 0;
    fflush(stdout);
}

/* ------------------------------------------------------------ SBI hooks */

static bool sbi_shutdown_cb(void *ud, u32 type, u32 reason) {
    vm *v = (vm *)ud;
    LOG_INFO("guest requested %s (reason %u) after %llu instructions",
             type == SBI_SRST_REBOOT ? "reboot" : "shutdown", reason, (unsigned long long)v->insns);
    vm_stop(v, reason);
    return false; /* stop the run loop */
}

static void sbi_set_timer_cb(void *ud, u64 stime) {
    vm *v = (vm *)ud;
    /* Program the CLINT comparator; the resulting MTIP is delegated to S-mode
     * as STIP by mideleg, which is what Linux expects. */
    v->clint.mtimecmp[0] = stime;
    v->cpu.csr[CSR_STIMECMP] = stime;
}

/* --------------------------------------------------------------- tracing */

static void trace_hook(void *ud, u64 pc, u32 insn, u32 len) {
    vm *v = (vm *)ud;
    LOG_TRACE("%012llx: %s%08x  priv=%u satp=%016llx", (unsigned long long)pc,
              len == 2 ? "    " : "", insn, v->cpu.priv, (unsigned long long)v->cpu.csr[CSR_SATP]);
    RVM_UNUSED(v);
}

/* ---------------------------------------------------------------- setup */

rvm_err vm_new(vm *v, const vm_opts *o) {
    if (!v || !o)
        return RVM_ERR_BADARG;
    memset(v, 0, sizeof(*v));
    v->opts = *o;
    rvm_log_set_level(o->log_level);

    rvm_err e;
    if ((e = bus_init(&v->bus, o->ram_size)) != RVM_OK)
        return e;
    if ((e = mmu_init(&v->mmu, &v->bus)) != RVM_OK)
        goto fail;
    if ((e = cpu_init(&v->cpu, &v->bus, &v->mmu, 0)) != RVM_OK)
        goto fail;
    if ((e = clint_init(&v->clint)) != RVM_OK)
        goto fail;
    if ((e = plic_init(&v->plic)) != RVM_OK)
        goto fail;
    if ((e = uart_init(&v->uart, RVM_UART_IRQ)) != RVM_OK)
        goto fail;
    sbi_init(&v->sbi, &v->bus);

    /* Counters are readable from every privilege level, which Linux' vdso
     * relies on for clock_gettime(). */
    v->cpu.csr[CSR_MCOUNTEREN] = 0xFFFFFFFFu;
    v->cpu.csr[CSR_SCOUNTEREN] = 0xFFFFFFFFu;
    v->cpu.csr[CSR_MENVCFG] = 0;

    bus_attach(&v->bus, "clint", RVM_CLINT_BASE, RVM_CLINT_SIZE, &v->clint, clint_load,
               clint_store);
    bus_attach(&v->bus, "plic", RVM_PLIC_BASE, RVM_PLIC_SIZE, &v->plic, plic_load, plic_store);
    bus_attach(&v->bus, "uart0", RVM_UART_BASE, RVM_UART_SIZE, &v->uart, uart_load, uart_store);

    uart_set_sink(&v->uart, uart_emit, v);
    uart_set_irq(&v->uart, irq_raise, v);

    /* virtio-blk on slot 0 when a disk image was requested. */
    if (o->disk_path && *o->disk_path) {
        e = virtio_blk_open(&v->blk, o->disk_path, o->disk_size, o->create_disk);
        if (e != RVM_OK)
            goto fail;
        v->blk_present = true;
        virtio_init(&v->vio[0], &v->bus, virtio_blk_backend(&v->blk), RVM_VIRTIO_IRQ(0));
        virtio_set_irq(&v->vio[0], irq_raise, v);
        bus_attach(&v->bus, "virtio0", RVM_VIRTIO_BASE, RVM_VIRTIO_STRIDE, &v->vio[0], virtio_load,
                   virtio_store);
    }

    /* Slots that have no backend still need to decode as "no device present"
     * so Linux' virtio_mmio probe does not fault: DeviceID reads 0. */
    for (u32 i = v->blk_present ? 1u : 0u; i < RVM_VIRTIO_COUNT; i++) {
        u64 base = RVM_VIRTIO_BASE + i * RVM_VIRTIO_STRIDE;
        bus_attach(&v->bus, "virtio-empty", base, RVM_VIRTIO_STRIDE, NULL, virtio_absent_load,
                   NULL);
    }

    v->sbi.write = NULL;
    v->sbi.write_ud = NULL;
    v->sbi.shutdown = sbi_shutdown_cb;
    v->sbi.shutdown_ud = v;
    v->sbi.set_timer = sbi_set_timer_cb;
    v->sbi.set_timer_ud = v;

    if (o->trace) {
        rvm_trace_from(o->trace_from);
        rvm_trace_set(trace_hook, v);
    }
    v->running = true;
    return RVM_OK;

fail:
    vm_free(v);
    return e;
}

/* ----------------------------------------------------------------- load */

rvm_err vm_load(vm *v) {
    const vm_opts *o = &v->opts;
    rvm_err e;

    if (!o->kernel_path || !*o->kernel_path)
        return RVM_ERR_BADARG;

    u64 entry = 0;
    if (o->raw_kernel) {
        u64 sz = 0;
        u64 addr = o->entry_override ? o->entry_override : RVM_KERNEL_ENTRY;
        e = loader_load_blob(&v->bus, o->kernel_path, addr, &sz);
        if (e != RVM_OK)
            return e;
        /* Linux' Image header carries its entry at offset 8. */
        u64 hdr_entry = 0;
        bus_load(&v->bus, addr + 8, 8, &hdr_entry);
        entry = hdr_entry ? addr + hdr_entry : addr;
    } else {
        loader_stats st;
        e = loader_load_elf(&v->bus, o->kernel_path, &entry, &st);
        if (e != RVM_OK)
            return e;
    }
    if (o->entry_override)
        entry = o->entry_override;
    v->entry = entry;

    /* ---- device tree ---- */
    u64 initrd_end = 0;
    if (o->initrd_path && *o->initrd_path) {
        u64 sz = 0;
        e = loader_load_blob(&v->bus, o->initrd_path, o->initrd_addr, &sz);
        if (e != RVM_OK)
            return e;
        initrd_end = o->initrd_addr + sz;
    }

    if (o->dtb_path && *o->dtb_path) {
        u64 sz = 0;
        e = loader_load_blob(&v->bus, o->dtb_path, o->dtb_addr, &sz);
        if (e != RVM_OK)
            return e;
        v->dtb_len = (u32)sz;
        LOG_INFO("vm: loaded external DTB (%llu bytes) at 0x%llx", (unsigned long long)sz,
                 (unsigned long long)o->dtb_addr);
    } else {
        dtb_opts d;
        memset(&d, 0, sizeof(d));
        d.ram_base = RVM_RAM_BASE;
        d.ram_size = o->ram_size;
        d.model = "rvm,virt";
        d.bootargs = o->bootargs;
        d.isa = o->isa;
        d.mmu_type = o->mmu_type;
        d.n_virtio = RVM_VIRTIO_COUNT;
        d.initrd_start = initrd_end ? o->initrd_addr : 0;
        d.initrd_end = initrd_end;
        d.timebase_hz = CLINT_TIMEBASE_HZ;
        d.stdout_path = "/soc/serial@10000000";
        e = dtb_build(&d, &v->dtb, &v->dtb_len);
        if (e != RVM_OK)
            return e;
        if (!bus_ram_valid(&v->bus, o->dtb_addr, v->dtb_len)) {
            free(v->dtb);
            return RVM_ERR_RANGE;
        }
        memcpy(bus_ram_ptr(&v->bus, o->dtb_addr), v->dtb, v->dtb_len);
        LOG_INFO("vm: built DTB in-process (%u bytes) at 0x%llx", v->dtb_len,
                 (unsigned long long)o->dtb_addr);
    }
    v->dtb_addr = o->dtb_addr;

    /* ---- hart state: enter S-mode like a real firmware payload hand-off ---- */
    cpu_reset(&v->cpu, entry, o->dtb_addr);
    v->cpu.priv = PRV_S;
    v->cpu.csr[CSR_MSTATUS] = (PRV_S << MSTATUS_MPP_SHIFT) | MSTATUS_MPIE;
    v->cpu.csr[CSR_MTVEC] = entry & ~3ULL; /* never fetched: traps are handled in C */
    LOG_INFO("vm: entry=0x%llx priv=S a0=%llu a1=0x%llx ram=%llu MiB", (unsigned long long)entry,
             (unsigned long long)v->cpu.x[10], (unsigned long long)v->cpu.x[11],
             (unsigned long long)(o->ram_size >> 20));
    return RVM_OK;
}

/* --------------------------------------------------------------- run */

/*
 * Polling host input is a read() syscall and refreshing the time base is a
 * clock_gettime(), so doing both on every retired instruction costs several
 * times more than the instruction itself -- measured at ~385 ns/instruction,
 * i.e. 2.6 MIPS, before this was throttled.  Refresh every RVM_POLL_INTERVAL
 * instructions *and* immediately whenever a device moves an interrupt line, so
 * latency is bounded by device activity rather than by the period.  256
 * instructions is far below a millisecond at any speed RVM reaches today, and
 * the CLINT's own MMIO read path advances mtime itself.
 */
#define RVM_POLL_INTERVAL 256u

/* Advance mtime and recompute what the guest sees in mip. */
static void refresh_interrupts(vm *v) {
    /* clint_tick() returns the MSIP/MTIP bits it drives -- *not* mtime.  The
     * two used to be conflated, which made CSR_TIME (rdtime) read as 0 and
     * broke the guest's clocksource. */
    u64 bits = clint_tick(&v->clint);
    v->cpu.hw_time = clint_mtime(&v->clint);
    v->cpu.hw_mip = bits | plic_update(&v->plic);
}

static void pump_input(vm *v) {
    if (!v->opts.poll)
        return;
    u8 buf[256];
    int n = v->opts.poll(v->opts.poll_ud, buf, sizeof(buf));
    for (int i = 0; i < n; i++)
        if (!uart_push(&v->uart, buf[i]))
            break;
}

static void idle_until_timer(vm *v) {
    /* WFI: sleep until the next comparator deadline, capped so host input and
     * device timers stay responsive. */
    u64 deadline = v->clint.mtimecmp[0];
    u64 now = v->clint.mtime;
    long ns;
    if (deadline == UINT64_MAX || deadline <= now) {
        ns = 1000000; /* 1 ms */
    } else {
        u64 ticks = deadline - now;
        u64 ns_per_tick = 1000000000ULL / CLINT_TIMEBASE_HZ;
        u64 want = ticks * ns_per_tick;
        ns = (long)RVM_MIN(want, 10000000ULL); /* cap at 10 ms */
    }
    struct timespec ts = {ns / 1000000000L, ns % 1000000000L};
    nanosleep(&ts, NULL);
}

rvm_err vm_run(vm *v) {
    if (!v->opts.kernel_path)
        return RVM_ERR_BADARG;
    v->start_ns = rvm_now_ns();
    v->running = true;

    while (v->running) {
        if (v->irq_dirty || (v->insns % RVM_POLL_INTERVAL) == 0) {
            pump_input(v);
            refresh_interrupts(v);
            v->irq_dirty = false;
        }

        step_result r = cpu_step(&v->cpu);
        v->insns++;

        switch (r) {
        case STEP_OK:
            break;

        case STEP_TRAP: {
            u64 cause = v->cpu.last_cause;
            bool is_irq = (cause >> 63) & 1;
            if (!is_irq && (cause & 0x3F) == EXC_ECALL_S && v->cpu.last_from_priv == PRV_S) {
                if (!sbi_handle(&v->sbi, &v->cpu))
                    goto done;
                cpu_mret(&v->cpu);
            } else if (is_irq && v->cpu.priv == PRV_M) {
                /* A non-delegated interrupt: acknowledge it and return. */
                cpu_mret(&v->cpu);
            }
            break;
        }

        case STEP_WFI:
            v->cpu.pc += v->cpu.last_insn_len ? v->cpu.last_insn_len : 4;
            v->cpu.instret++;
            /* hw_mip can be up to RVM_POLL_INTERVAL instructions stale, and
             * going to sleep on a stale "nothing pending" is the one place
             * where that would actually hang the guest. */
            pump_input(v);
            refresh_interrupts(v);
            v->irq_dirty = false;
            if (!(v->cpu.hw_mip & v->cpu.csr[CSR_MIE]))
                idle_until_timer(v);
            break;

        case STEP_ECALL_M:
            LOG_INFO("vm: ecall from M-mode (a0=0x%llx a7=0x%llx) -- stopping",
                     (unsigned long long)v->cpu.x[10], (unsigned long long)v->cpu.x[17]);
            v->exit_code = (u32)v->cpu.x[10];
            goto done;

        case STEP_SHUTDOWN:
            goto done;

        case STEP_FAULT:
            LOG_ERROR("vm: emulator-level fault at pc=0x%llx", (unsigned long long)v->cpu.pc);
            uart_flush(v);
            return RVM_ERR;

        default:
            break;
        }

        if (v->opts.max_insns && v->insns >= v->opts.max_insns) {
            LOG_INFO("vm: instruction budget of %llu reached",
                     (unsigned long long)v->opts.max_insns);
            v->exit_code = 0;
            goto done;
        }
    }

done:
    uart_flush(v);
    v->running = false;
    return RVM_OK;
}

void vm_stop(vm *v, u32 code) {
    v->running = false;
    v->exit_code = code;
}

void vm_print_stats(const vm *v) {
    u64 elapsed = rvm_now_ns() - v->start_ns;
    double secs = elapsed ? (double)elapsed / 1e9 : 1e-9;
    /* Written out long hand: instructions per second, divided by a million.
     * Instructions per nanosecond is *not* MIPS -- that is off by 1000x, and
     * PLAN step 12 measures the JIT against this number. */
    double mips = (elapsed > 0) ? (double)v->insns / secs / 1e6 : 0.0;
    fprintf(stderr,
            "\n--- RVM statistics -------------------------------------------\n"
            "  instructions retired : %llu\n"
            "  wall time            : %.3f s\n"
            "  throughput           : %.2f MIPS\n"
            "  traps                : %llu (ecalls %llu, SBI %llu, unsupported %llu)\n"
            "  RAM  loads/stores    : %llu / %llu\n"
            "  MMIO loads/stores    : %llu / %llu (%llu faults)\n"
            "  TLB  hit/miss/walk   : %llu / %llu / %llu (%llu faults)\n"
            "  UART tx/rx           : %llu / %llu\n",
            (unsigned long long)v->insns, secs, mips, (unsigned long long)v->cpu.n_traps,
            (unsigned long long)v->cpu.n_ecalls, (unsigned long long)v->sbi.n_calls,
            (unsigned long long)v->sbi.n_unsupported, (unsigned long long)v->bus.n_ram_load,
            (unsigned long long)v->bus.n_ram_store, (unsigned long long)v->bus.n_mmio_load,
            (unsigned long long)v->bus.n_mmio_store, (unsigned long long)v->bus.n_fault,
            (unsigned long long)v->mmu.n_tlb_hit, (unsigned long long)v->mmu.n_tlb_miss,
            (unsigned long long)v->mmu.n_walk, (unsigned long long)v->mmu.n_fault,
            (unsigned long long)v->uart.n_tx, (unsigned long long)v->uart.n_rx);
    if (v->blk_present)
        fprintf(stderr, "  virtio-blk r/w/flush : %llu / %llu / %llu (%llu errors)\n",
                (unsigned long long)v->blk.n_read, (unsigned long long)v->blk.n_write,
                (unsigned long long)v->blk.n_flush, (unsigned long long)v->blk.n_err);
    fprintf(stderr, "  PLIC raise/claim     : %llu / %llu\n", (unsigned long long)v->plic.n_raise,
            (unsigned long long)v->plic.n_claim);
    fprintf(stderr, "--------------------------------------------------------------\n");
}

void vm_free(vm *v) {
    if (!v)
        return;
    rvm_trace_set(NULL, NULL);
    if (v->blk_present)
        virtio_blk_close(&v->blk);
    free(v->dtb);
    bus_free(&v->bus);
    memset(v, 0, sizeof(*v));
}

void vm_console_in(vm *v, const u8 *buf, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!uart_push(&v->uart, buf[i]))
            break;
}
