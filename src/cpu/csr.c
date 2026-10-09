/*
 * csr.c -- control & status registers, trap entry and interrupt priority.
 *
 * The alias CSRs (sstatus/sie/sip) are views of the machine registers masked
 * by mideleg, exactly as the privileged spec requires, so there is a single
 * source of truth and no chance of the two drifting apart.
 *
 * SPDX-License-Identifier: MIT
 */
#include "cpu.h"

/* ---------------------------------------------------------------- naming */

const char *csr_name(u32 a)
{
    switch (a) {
    case CSR_MSTATUS: return "mstatus";
    case CSR_MISA: return "misa";
    case CSR_MEDELEG: return "medeleg";
    case CSR_MIDELEG: return "mideleg";
    case CSR_MIE: return "mie";
    case CSR_MTVEC: return "mtvec";
    case CSR_MCOUNTEREN: return "mcounteren";
    case CSR_MSCRATCH: return "mscratch";
    case CSR_MEPC: return "mepc";
    case CSR_MCAUSE: return "mcause";
    case CSR_MTVAL: return "mtval";
    case CSR_MIP: return "mip";
    case CSR_MHARTID: return "mhartid";
    case CSR_MVENDORID: return "mvendorid";
    case CSR_MARCHID: return "marchid";
    case CSR_MIMPID: return "mimpid";
    case CSR_MCYCLE: return "mcycle";
    case CSR_MINSTRET: return "minstret";
    case CSR_SSTATUS: return "sstatus";
    case CSR_SIE: return "sie";
    case CSR_STVEC: return "stvec";
    case CSR_SCOUNTEREN: return "scounteren";
    case CSR_SSCRATCH: return "sscratch";
    case CSR_SEPC: return "sepc";
    case CSR_SCAUSE: return "scause";
    case CSR_STVAL: return "stval";
    case CSR_SIP: return "sip";
    case CSR_STIMECMP: return "stimecmp";
    case CSR_SATP: return "satp";
    case CSR_FFLAGS: return "fflags";
    case CSR_FRM: return "frm";
    case CSR_FCSR: return "fcsr";
    case CSR_CYCLE: return "cycle";
    case CSR_TIME: return "time";
    case CSR_INSTRET: return "instret";
    default: return NULL;
    }
}

/* mstatus.SD is a read-only summary of FS/XS; recompute it on every read. */
static u64 mstatus_with_sd(u64 v)
{
    bool dirty = ((v & MSTATUS_FS) == (3ULL << MSTATUS_FS_SHIFT)) ||
                 ((v & MSTATUS_XS) == (3ULL << MSTATUS_XS_SHIFT));
    return dirty ? (v | MSTATUS_SD) : (v & ~MSTATUS_SD);
}

/* ------------------------------------------------------------------ read */

u64 csr_read(cpu *c, u32 a, bool *illegal)
{
    if (illegal) *illegal = false;
    /* Bits [11:10] encode read/write-ness: 0b11 means read-only. */
    if (((a >> 10) & 3) == 3) { /* read-only CSR: reads are always legal */
    }

    switch (a) {
    case CSR_MSTATUS: return mstatus_with_sd(c->csr[CSR_MSTATUS]);
    case CSR_SSTATUS: return mstatus_with_sd(c->csr[CSR_MSTATUS]) & SSTATUS_MASK;

    case CSR_MISA: return c->csr[CSR_MISA];

    case CSR_MIE: return c->csr[CSR_MIE];
    case CSR_SIE: return c->csr[CSR_MIE] & c->csr[CSR_MIDELEG];

    case CSR_MIP: return c->csr[CSR_MIP] | c->hw_mip;
    case CSR_SIP: return (c->csr[CSR_MIP] | c->hw_mip) & c->csr[CSR_MIDELEG];

    case CSR_TIME: return c->hw_time;
    case CSR_CYCLE: return c->cycles;
    case CSR_INSTRET: return c->instret;
    case CSR_MCYCLE: return c->cycles;
    case CSR_MINSTRET: return c->instret;

    case CSR_FFLAGS: return c->csr[CSR_FCSR] & 0x1F;
    case CSR_FRM: return (c->csr[CSR_FCSR] >> 5) & 0x7;
    case CSR_FCSR: return c->csr[CSR_FCSR] & 0xFF;

    case CSR_MHARTID: return c->hartid;
    case CSR_MVENDORID: return 0;
    case CSR_MARCHID: return 0x72766dULL; /* "rvm" */
    case CSR_MIMPID: return 1;

    /* Sstc: stimecmp is emulated through the CLINT, stored plainly. */
    case CSR_STIMECMP: return c->csr[CSR_STIMECMP];

    default:
        if (a < 4096) return c->csr[a];
        if (illegal) *illegal = true;
        return 0;
    }
}

/* ----------------------------------------------------------------- write */

void csr_write(cpu *c, u32 a, u64 v, bool *illegal)
{
    if (illegal) *illegal = false;
    if (((a >> 10) & 3) == 3) { /* read-only CSR */
        if (illegal) *illegal = true;
        return;
    }

    switch (a) {
    case CSR_MSTATUS:
        /* WARL: keep only the fields we implement; MPP is clamped to <= M. */
        c->csr[CSR_MSTATUS] = v & ~(MSTATUS_MPP);
        c->csr[CSR_MSTATUS] |= (v & MSTATUS_MPP);
        break;

    case CSR_SSTATUS:
        c->csr[CSR_MSTATUS] = (c->csr[CSR_MSTATUS] & ~SSTATUS_MASK) | (v & SSTATUS_MASK);
        break;

    case CSR_MISA:
        /* Writes are ignored: the ISA string is fixed at RV64IMAFDCSU. */
        break;

    case CSR_MIE: c->csr[CSR_MIE] = v & 0x2AAA; break;
    case CSR_SIE: {
        u64 d = c->csr[CSR_MIDELEG];
        c->csr[CSR_MIE] = (c->csr[CSR_MIE] & ~d) | (v & d);
        break;
    }

    case CSR_MIP:
        /* Only SSIP is software-writable through mip. */
        c->csr[CSR_MIP] = (c->csr[CSR_MIP] & ~MIP_SSIP) | (v & MIP_SSIP);
        break;
    case CSR_SIP: {
        u64 d = c->csr[CSR_MIDELEG] & MIP_SSIP;
        c->csr[CSR_MIP] = (c->csr[CSR_MIP] & ~d) | (v & d);
        break;
    }

    case CSR_SATP: {
        /* TVM: S-mode access to satp (and sfence.vma) traps when mstatus.TVM. */
        if (c->priv == PRV_S && (c->csr[CSR_MSTATUS] & MSTATUS_TVM)) {
            if (illegal) *illegal = true;
            return;
        }
        u64 mode = (v >> 60) & 0xF;
        if (mode != SATP_MODE_BARE && !((mode == SATP_MODE_SV39) || (mode == SATP_MODE_SV48) ||
                                        (mode == SATP_MODE_SV57))) {
            break; /* WARL: ignore unsupported modes */
        }
        bool changed = (c->csr[CSR_SATP] != v);
        c->csr[CSR_SATP] = v;
        if (changed && c->mmu) mmu_flush(c->mmu);
        break;
    }

    case CSR_FFLAGS: c->csr[CSR_FCSR] = (c->csr[CSR_FCSR] & ~0x1FU) | (v & 0x1F); break;
    case CSR_FRM: c->csr[CSR_FCSR] = (c->csr[CSR_FCSR] & ~0xE0U) | ((v & 0x7) << 5); break;
    case CSR_FCSR: c->csr[CSR_FCSR] = v & 0xFF; break;

    case CSR_MEPC: c->csr[CSR_MEPC] = v & ~1ULL; break;
    case CSR_SEPC: c->csr[CSR_SEPC] = v & ~1ULL; break;

    case CSR_MCYCLE: c->cycles = v; break;
    case CSR_MINSTRET: c->instret = v; break;

    default:
        if (a < 4096) {
            c->csr[a] = v;
        } else if (illegal) {
            *illegal = true;
        }
        break;
    }
}

/* ------------------------------------------------------------- interrupts */

static const u32 irq_priority[] = {IRQ_M_EXT, IRQ_M_SOFT, IRQ_M_TIMER,
                                   IRQ_S_EXT, IRQ_S_SOFT, IRQ_S_TIMER};

u64 cpu_pending_interrupt(cpu *c)
{
    u64 mip = c->csr[CSR_MIP] | c->hw_mip;
    u64 mie = c->csr[CSR_MIE];
    u64 mideleg = c->csr[CSR_MIDELEG];
    u64 pending = mip & mie;
    if (!pending) return 0;

    u64 to_m = pending & ~mideleg;
    u64 to_s = pending & mideleg;
    u64 take = 0;

    if (c->priv < PRV_M) {
        take |= to_m; /* higher privilege always preempts */
    } else if (c->csr[CSR_MSTATUS] & MSTATUS_MIE) {
        take |= to_m;
    }
    if (c->priv < PRV_S) {
        take |= to_s;
    } else if (c->priv == PRV_S && (c->csr[CSR_MSTATUS] & MSTATUS_SIE)) {
        take |= to_s;
    }
    if (!take) return 0;

    for (u32 i = 0; i < RVM_ARRAY_SIZE(irq_priority); i++) {
        u32 b = irq_priority[i];
        if (take & (1ULL << b)) return (1ULL << 63) | b;
    }
    return 0;
}

/* ------------------------------------------------------------------ traps */

static u64 tvec_target(u64 tvec, u64 cause, bool interrupt)
{
    u64 base = tvec & ~3ULL;
    u32 mode = (u32)(tvec & 3);
    if (mode == 1 && interrupt) return base + 4 * (cause & 0xF);
    return base; /* direct, or mode 1 with an exception */
}

void cpu_trap(cpu *c, u64 cause, u64 tval, bool interrupt)
{
    bool deleg = false;
    if (c->priv <= PRV_S) {
        u64 mask = interrupt ? c->csr[CSR_MIDELEG] : c->csr[CSR_MEDELEG];
        if (cause < 64 && (mask & (1ULL << cause))) deleg = true;
    }

    c->n_traps++;
    c->last_cause = cause | (interrupt ? (1ULL << 63) : 0);
    c->last_tval = tval;
    c->last_from_priv = c->priv;

    u64 *status = &c->csr[CSR_MSTATUS];

    if (deleg) {
        c->csr[CSR_SEPC] = c->pc & ~1ULL;
        c->csr[CSR_SCAUSE] = cause | (interrupt ? (1ULL << 63) : 0);
        c->csr[CSR_STVAL] = tval;
        *status = (*status & ~MSTATUS_SPIE) | ((*status & MSTATUS_SIE) ? MSTATUS_SPIE : 0);
        *status &= ~MSTATUS_SIE;
        *status = (*status & ~MSTATUS_SPP) | ((c->priv == PRV_S) ? MSTATUS_SPP : 0);
        c->priv = PRV_S;
        c->pc = tvec_target(c->csr[CSR_STVEC], cause, interrupt);
    } else {
        c->csr[CSR_MEPC] = c->pc & ~1ULL;
        c->csr[CSR_MCAUSE] = cause | (interrupt ? (1ULL << 63) : 0);
        c->csr[CSR_MTVAL] = tval;
        *status = (*status & ~MSTATUS_MPIE) | ((*status & MSTATUS_MIE) ? MSTATUS_MPIE : 0);
        *status &= ~MSTATUS_MIE;
        *status = (*status & ~MSTATUS_MPP) | ((u64)c->priv << MSTATUS_MPP_SHIFT);
        c->priv = PRV_M;
        c->pc = tvec_target(c->csr[CSR_MTVEC], cause, interrupt);
    }
    LOG_TRACE("trap: cause=%llu tval=0x%llx %s -> %s-mode pc=0x%llx",
              (unsigned long long)(cause | (interrupt ? (1ULL << 63) : 0)),
              (unsigned long long)tval, interrupt ? "irq" : "exc", deleg ? "S" : "M",
              (unsigned long long)c->pc);
}

void cpu_mret(cpu *c)
{
    u64 *s = &c->csr[CSR_MSTATUS];
    u64 mpp = (*s & MSTATUS_MPP) >> MSTATUS_MPP_SHIFT;
    c->priv = (u32)mpp;
    *s = (*s & ~MSTATUS_MIE) | ((*s & MSTATUS_MPIE) ? MSTATUS_MIE : 0);
    *s |= MSTATUS_MPIE;
    *s &= ~MSTATUS_MPP; /* MPP <- U, the least privileged mode we support */
    c->pc = c->csr[CSR_MEPC];
}

void cpu_sret(cpu *c)
{
    u64 *s = &c->csr[CSR_MSTATUS];
    u64 spp = (*s & MSTATUS_SPP) ? PRV_S : PRV_U;
    c->priv = (u32)spp;
    *s = (*s & ~MSTATUS_SIE) | ((*s & MSTATUS_SPIE) ? MSTATUS_SIE : 0);
    *s |= MSTATUS_SPIE;
    *s &= ~MSTATUS_SPP;
    c->pc = c->csr[CSR_SEPC];
}

/* ----------------------------------------------------------------- reset */

void cpu_init_isa(cpu *c)
{
    /* MXL = 2 (RV64) plus I M A F D C S U. */
    u64 ext = (1ULL << 8) |  /* I */
              (1ULL << 12) | /* M */
              (1ULL << 0) |  /* A */
              (1ULL << 5) |  /* F */
              (1ULL << 3) |  /* D */
              (1ULL << 2) |  /* C */
              (1ULL << 18) | /* S */
              (1ULL << 20);  /* U */
    c->csr[CSR_MISA] = (2ULL << 62) | ext;
}
