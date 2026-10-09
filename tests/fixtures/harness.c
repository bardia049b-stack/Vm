/* SPDX-License-Identifier: MIT */
#include "harness.h"

static void th_emit_char(void *ud, u8 ch) {
    th *t = (th *)ud;
    if (t->outlen < sizeof(t->out))
        t->out[t->outlen++] = ch;
}

static void th_raise(void *ud, u32 irq, bool level) {
    th *t = (th *)ud;
    if (level)
        plic_raise(&t->plic, irq);
}

rvm_err th_init(th *t) {
    rvm_err e;
    memset(t, 0, sizeof(*t));
    if ((e = bus_init(&t->bus, 16ULL << 20)) != RVM_OK)
        return e;
    if ((e = mmu_init(&t->mmu, &t->bus)) != RVM_OK)
        return e;
    if ((e = cpu_init(&t->cpu, &t->bus, &t->mmu, 0)) != RVM_OK)
        return e;
    if ((e = clint_init(&t->clint)) != RVM_OK)
        return e;
    if ((e = plic_init(&t->plic)) != RVM_OK)
        return e;
    if ((e = uart_init(&t->uart, RVM_UART_IRQ)) != RVM_OK)
        return e;

    uart_set_sink(&t->uart, th_emit_char, t);
    uart_set_irq(&t->uart, th_raise, t);
    bus_attach(&t->bus, "uart0", RVM_UART_BASE, RVM_UART_SIZE, &t->uart, uart_load, uart_store);
    bus_attach(&t->bus, "clint", RVM_CLINT_BASE, RVM_CLINT_SIZE, &t->clint, clint_load,
               clint_store);
    bus_attach(&t->bus, "plic", RVM_PLIC_BASE, RVM_PLIC_SIZE, &t->plic, plic_load, plic_store);

    t->code = TH_CODE_ADDR;
    t->cpu.pc = TH_CODE_ADDR;
    t->cpu.priv = PRV_M;
    t->cpu.csr[CSR_MSTATUS] = 3ULL << MSTATUS_FS_SHIFT; /* FP on, so F/D tests run */
    return RVM_OK;
}

void th_free(th *t) {
    if (!t)
        return;
    bus_free(&t->bus);
    memset(t, 0, sizeof(*t));
}

u64 th_emit(th *t, u32 insn) {
    u64 a = t->code;
    memcpy(bus_ram_ptr(&t->bus, a), &insn, 4);
    t->code += 4;
    return a;
}

u64 th_emit16(th *t, u16 insn) {
    u64 a = t->code;
    memcpy(bus_ram_ptr(&t->bus, a), &insn, 2);
    t->code += 2;
    return a;
}

void th_run(th *t, u32 n) {
    for (u32 i = 0; i < n; i++) {
        t->cpu.hw_time = clint_tick(&t->clint);
        t->cpu.hw_mip = plic_update(&t->plic);
        if (t->clint.mtime >= t->clint.mtimecmp[0])
            t->cpu.hw_mip |= MIP_MTIP;
        step_result r = cpu_step(&t->cpu);
        t->steps++;
        if (r == STEP_TRAP && t->cpu.last_cause == EXC_ECALL_S) {
            /* Tests that exercise SBI do it explicitly; stop the loop here. */
            break;
        }
        if (r == STEP_FAULT || r == STEP_SHUTDOWN)
            break;
        if (r == STEP_WFI) {
            t->cpu.pc += t->cpu.last_insn_len ? t->cpu.last_insn_len : 4;
        }
    }
}

void th_run_all(th *t) {
    for (u32 i = 0; i < 8192; i++) {
        if (t->cpu.pc >= t->code)
            return; /* every emitted instruction has run */
        t->cpu.hw_time = clint_tick(&t->clint);
        t->cpu.hw_mip = plic_update(&t->plic);
        if (t->clint.mtime >= t->clint.mtimecmp[0])
            t->cpu.hw_mip |= MIP_MTIP;
        step_result r = cpu_step(&t->cpu);
        t->steps++;
        if (r == STEP_FAULT || r == STEP_SHUTDOWN || r == STEP_ECALL_M)
            return;
        if (r == STEP_WFI)
            t->cpu.pc += t->cpu.last_insn_len ? t->cpu.last_insn_len : 4;
    }
}

void th_li(th *t, u32 rd, u32 v) {
    /* Small non-negative values fit in one addi. */
    if (v < 0x800u) {
        th_emit(t, ADDI(rd, X_ZERO, (s32)v));
        return;
    }
    u32 hi = v & 0xFFFFF000u;
    s32 lo = (s32)(v & 0xFFFu);
    if (lo < 0)
        hi = (u32)((s64)hi + 0x1000); /* the addi sign-extends, so round up */
    th_emit(t, LUI(rd, hi));
    if (lo != 0)
        th_emit(t, ADDI(rd, rd, lo));
    if (v & 0x80000000u) {
        /* lui sign-extends bit 31 into bits 63:32; shift left then right to
         * zero them again, leaving the exact 32-bit value. */
        th_emit(t, SLLI(rd, rd, 32));
        th_emit(t, SRLI(rd, rd, 32));
    }
}

void th_poke64(th *t, u64 a, u64 v) {
    memcpy(bus_ram_ptr(&t->bus, a), &v, 8);
}
u64 th_peek64(th *t, u64 a) {
    u64 v = 0;
    memcpy(&v, bus_ram_ptr(&t->bus, a), 8);
    return v;
}
void th_poke32(th *t, u64 a, u32 v) {
    memcpy(bus_ram_ptr(&t->bus, a), &v, 4);
}
u32 th_peek32(th *t, u64 a) {
    u32 v = 0;
    memcpy(&v, bus_ram_ptr(&t->bus, a), 4);
    return v;
}
