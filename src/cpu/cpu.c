/*
 * cpu.c -- fetch / decode / execute for RV64IMA plus the SYSTEM opcode.
 *
 * The decoder is one big switch on the major opcode, which gcc turns into a
 * jump table: ~2 indirect branches per instruction and no dynamic allocation
 * anywhere on the hot path.  F/D live in fp.c and the C extension is expanded
 * to its canonical 32-bit form in compressed.c so there is exactly one
 * execution path for every instruction.
 *
 * SPDX-License-Identifier: MIT
 */
#include "cpu.h"

/* ------------------------------------------------------------- lifecycle */

rvm_err cpu_init(cpu *c, bus *b, mmu *m, u32 hartid) {
    if (!c || !b || !m)
        return RVM_ERR_BADARG;
    memset(c, 0, sizeof(*c));
    c->bus = b;
    c->mmu = m;
    c->hartid = hartid;
    c->priv = PRV_M;
    cpu_init_isa(c);
    return RVM_OK;
}

/*
 * Reset into the state a real M-mode firmware would hand to the payload:
 * everything zeroed, interrupts off, and medeleg/mideleg pre-populated so an
 * S-mode kernel sees its own faults.  `dtb_addr` goes into a1 per the RISC-V
 * boot protocol (a0 = hartid, a1 = DTB).
 */
void cpu_reset(cpu *c, u64 entry_pc, u64 dtb_addr) {
    u64 misa = c->csr[CSR_MISA];
    u64 hartid = c->hartid;
    bus *b = c->bus;
    mmu *m = c->mmu;
    memset(c, 0, sizeof(*c));
    c->bus = b;
    c->mmu = m;
    c->hartid = hartid;
    c->csr[CSR_MISA] = misa;

    c->pc = entry_pc;
    c->priv = PRV_M;
    c->x[10] = hartid;   /* a0 */
    c->x[11] = dtb_addr; /* a1 */

    c->csr[CSR_MSTATUS] = 0;
    c->csr[CSR_MEDELEG] = RVM_MEDELEG_DEFAULT;
    c->csr[CSR_MIDELEG] = RVM_MIDELEG_DEFAULT;
    c->csr[CSR_MTVEC] = entry_pc; /* replaced by the built-in shim, see vm.c */
    c->csr[CSR_SATP] = 0;
    c->csr[CSR_MIE] = 0;
    c->csr[CSR_MIP] = 0;
    c->csr[CSR_STIMECMP] = UINT64_MAX;
    mmu_flush(m);
}

/* --------------------------------------------------- address translation */

static u32 eff_priv_for_mem(const cpu *c) {
    if (c->priv == PRV_M) {
        if (c->csr[CSR_MSTATUS] & MSTATUS_MPRV)
            return (u32)((c->csr[CSR_MSTATUS] & MSTATUS_MPP) >> MSTATUS_MPP_SHIFT);
        return PRV_M; /* bare */
    }
    return c->priv;
}

static bool translate(cpu *c, u64 va, u32 acc, u64 *pa, u32 *cause) {
    u32 ep = eff_priv_for_mem(c);
    if (ep == PRV_M) {
        *pa = va;
        return true;
    }
    u64 mst = c->csr[CSR_MSTATUS];
    mmu_xlat r = mmu_translate(c->mmu, va, ep, c->csr[CSR_SATP], (mst & MSTATUS_MXR) != 0,
                               (mst & MSTATUS_SUM) != 0, acc);
    if (!r.ok) {
        if (cause)
            *cause = r.cause;
        return false;
    }
    *pa = r.pa;
    return true;
}

/*
 * Load/store with translation.  Accesses that straddle a page boundary are
 * split so each half is translated independently -- required for correctness
 * when the two halves live in different physical pages.
 */
bool cpu_mem_load(cpu *c, u64 va, u32 size, u64 *out, u32 *cause) {
    u64 pa;
    if (!translate(c, va, ACC_LOAD, &pa, cause))
        return false;
    u64 page_off = pa & 4095;
    if (page_off + size <= 4096) {
        if (bus_load(c->bus, pa, size, out))
            return true;
        *cause = EXC_LOAD_FAULT;
        return false;
    }

    /* straddling: assemble from per-page halves */
    u64 v = 0;
    u32 done = 0;
    while (done < size) {
        u32 chunk = (u32)RVM_MIN((u64)(size - done), 4096 - ((pa + done) & 4095));
        u64 p;
        if (!translate(c, va + done, ACC_LOAD, &p, cause))
            return false;
        u64 part = 0;
        if (!bus_load(c->bus, p, chunk, &part)) {
            *cause = EXC_LOAD_FAULT;
            return false;
        }
        v |= part << (8 * done);
        done += chunk;
    }
    *out = v;
    return true;
}

bool cpu_mem_store(cpu *c, u64 va, u32 size, u64 val, u32 *cause) {
    u64 pa;
    if (!translate(c, va, ACC_STORE, &pa, cause))
        return false;
    if ((pa & 4095) + size <= 4096) {
        if (bus_store(c->bus, pa, size, val))
            return true;
        *cause = EXC_STORE_FAULT;
        return false;
    }

    u32 done = 0;
    while (done < size) {
        u32 chunk = (u32)RVM_MIN((u64)(size - done), 4096 - ((pa + done) & 4095));
        u64 p;
        if (!translate(c, va + done, ACC_STORE, &p, cause))
            return false;
        if (!bus_store(c->bus, p, chunk, (val >> (8 * done)))) {
            *cause = EXC_STORE_FAULT;
            return false;
        }
        done += chunk;
    }
    return true;
}

static inline s64 sext_load(u64 v, u32 size, bool is_unsigned) {
    if (is_unsigned)
        return (s64)v;
    switch (size) {
    case 1:
        return (s64)(s8)(u8)v;
    case 2:
        return (s64)(s16)(u16)v;
    case 4:
        return (s64)(s32)(u32)v;
    default:
        return (s64)v;
    }
}

/* ------------------------------------------------------------------ fetch */

static bool fetch(cpu *c, u32 *insn, u32 *len, u32 *cause) {
    u64 pa;
    if (!translate(c, c->pc, ACC_EXEC, &pa, cause))
        return false;
    u64 lo = 0;
    if (!bus_load(c->bus, pa, 2, &lo)) {
        *cause = EXC_INST_FAULT;
        return false;
    }
    if ((lo & 3) != 3) { /* compressed */
        *insn = (u32)(u16)lo;
        *len = 2;
        return true;
    }
    if ((pa & 4095) == 4094) {
        /* 32-bit instruction crossing a page boundary: fetch the tail
         * separately so translation is checked for both pages. */
        u64 pa2;
        if (!translate(c, c->pc + 2, ACC_EXEC, &pa2, cause))
            return false;
        u64 hi = 0;
        if (!bus_load(c->bus, pa2, 2, &hi)) {
            *cause = EXC_INST_FAULT;
            return false;
        }
        *insn = (u32)(lo | (hi << 16));
        *len = 4;
        return true;
    }
    u64 full = 0;
    if (!bus_load(c->bus, pa, 4, &full)) {
        *cause = EXC_INST_FAULT;
        return false;
    }
    *insn = (u32)full;
    *len = 4;
    return true;
}

/* ------------------------------------------------------------- decode bits */

#define RD(i)  ((u32)((i) >> 7) & 0x1F)
#define RS1(i) ((u32)((i) >> 15) & 0x1F)
#define RS2(i) ((u32)((i) >> 20) & 0x1F)
#define F3(i)  ((u32)((i) >> 12) & 0x7)
#define F7(i)  ((u32)((i) >> 25) & 0x7F)
#define OP(i)  ((u32)(i)&0x7F)

static inline s32 imm_i(u32 i) {
    return sext((s32)i >> 20, 12);
}
static inline s32 imm_s(u32 i) {
    return sext((s32)(((i >> 25) << 5) | ((i >> 7) & 0x1F)), 12);
}
/* imm[12]=insn[31], imm[11]=insn[7], imm[10:5]=insn[30:25], imm[4:1]=insn[11:8] */
static inline s32 imm_b(u32 i) {
    u32 v = (((i >> 31) & 1) << 12) | (((i >> 7) & 1) << 11) | (((i >> 25) & 0x3F) << 5) |
            (((i >> 8) & 0xF) << 1);
    return sext((s32)v, 13) & ~1;
}
static inline s32 imm_u(u32 i) {
    return (s32)(i & 0xFFFFF000u);
}
/* imm[20]=insn[31], imm[19:12]=insn[19:12], imm[11]=insn[20], imm[10:1]=insn[30:21] */
static inline s32 imm_j(u32 i) {
    u32 v = (((i >> 31) & 1) << 20) | (((i >> 12) & 0xFF) << 12) | (((i >> 20) & 1) << 11) |
            (((i >> 21) & 0x3FF) << 1);
    return sext((s32)v, 21) & ~1;
}

/* --------------------------------------------------------------- execute */

#define TRAP_ILLEGAL()                                                                             \
    do {                                                                                           \
        cpu_trap(c, EXC_ILLEGAL_INST, insn, false);                                                \
        *res = STEP_TRAP;                                                                          \
        return false;                                                                              \
    } while (0)

static void do_amo(cpu *c, u32 insn, u32 size, step_result *res, bool *ok);

bool cpu_exec32(cpu *c, u32 insn, step_result *res) {
    *res = STEP_OK;
    u32 rd = RD(insn), rs1 = RS1(insn), rs2 = RS2(insn), f3 = F3(insn), f7 = F7(insn);
    u64 a = cpu_rd(c, rs1), b = cpu_rd(c, rs2);
    s64 sa = (s64)a, sb = (s64)b;
    u64 npc = c->pc; /* next pc if the instruction does not branch */
    bool taken = false;

    switch (OP(insn)) {
    /* ------------------------------------------------------------ LUI */
    case 0x37:
        cpu_wr(c, rd, (u64)(s64)imm_u(insn));
        break;

    /* ---------------------------------------------------------- AUIPC */
    case 0x17:
        cpu_wr(c, rd, c->pc + (u64)(s64)imm_u(insn));
        break;

    /* ------------------------------------------------------------ JAL */
    case 0x6F: {
        s32 off = imm_j(insn);
        cpu_wr(c, rd, c->pc + (c->last_insn_len ? c->last_insn_len : 4));
        npc = c->pc + off;
        taken = true;
        break;
    }

    /* ----------------------------------------------------------- JALR */
    case 0x67: {
        if (f3 != 0)
            TRAP_ILLEGAL();
        u64 t = c->pc + (c->last_insn_len ? c->last_insn_len : 4);
        npc = (a + (u64)(s64)imm_i(insn)) & ~1ULL;
        cpu_wr(c, rd, t);
        taken = true;
        break;
    }

    /* --------------------------------------------------------- BRANCH */
    case 0x63: {
        s32 off = imm_b(insn);
        bool t = false;
        switch (f3) {
        case 0x0:
            t = (a == b);
            break;
        case 0x1:
            t = (a != b);
            break;
        case 0x4:
            t = (sa < sb);
            break;
        case 0x5:
            t = (sa >= sb);
            break;
        case 0x6:
            t = (a < b);
            break;
        case 0x7:
            t = (a >= b);
            break;
        default:
            TRAP_ILLEGAL();
        }
        if (t) {
            npc = c->pc + off;
            taken = true;
        }
        break;
    }

    /* ----------------------------------------------------------- LOAD */
    case 0x03: {
        u32 size;
        bool uns = false;
        switch (f3) {
        case 0x0:
            size = 1;
            break;
        case 0x1:
            size = 2;
            break;
        case 0x2:
            size = 4;
            break;
        case 0x3:
            size = 8;
            break;
        case 0x4:
            size = 1;
            uns = true;
            break;
        case 0x5:
            size = 2;
            uns = true;
            break;
        case 0x6:
            size = 4;
            uns = true;
            break;
        default:
            TRAP_ILLEGAL();
        }
        u64 v = 0;
        u32 cause = 0;
        u64 addr = a + (u64)(s64)imm_i(insn);
        if (!cpu_mem_load(c, addr, size, &v, &cause)) {
            cpu_trap(c, cause, addr, false);
            *res = STEP_TRAP;
            return false;
        }
        cpu_wr(c, rd, (u64)sext_load(v, size, uns));
        break;
    }

    /* ---------------------------------------------------------- STORE */
    case 0x23: {
        u32 size;
        switch (f3) {
        case 0x0:
            size = 1;
            break;
        case 0x1:
            size = 2;
            break;
        case 0x2:
            size = 4;
            break;
        case 0x3:
            size = 8;
            break;
        default:
            TRAP_ILLEGAL();
        }
        u32 cause = 0;
        u64 addr = a + (u64)(s64)imm_s(insn);
        if (!cpu_mem_store(c, addr, size, b, &cause)) {
            cpu_trap(c, cause, addr, false);
            *res = STEP_TRAP;
            return false;
        }
        break;
    }

    /* -------------------------------------------------------- OP-IMM */
    case 0x13: {
        s64 imm = imm_i(insn);
        u64 r = 0;
        if (f3 == 0x1 || f3 == 0x5) {
            /* RV64: the shift amount is imm[5:0], so insn[25] belongs to the
             * amount and the discriminator is funct6 = insn[31:26].
             * 0b000000 = SLLI/SRLI, 0b010000 = SRAI. */
            u32 sh = (u32)(insn >> 20) & 0x3F;
            u32 f6 = (u32)(insn >> 26) & 0x3F;
            if (f3 == 0x1) {
                if (f6 != 0x00)
                    TRAP_ILLEGAL();
                r = a << sh;
            } else {
                if (f6 != 0x00 && f6 != 0x10)
                    TRAP_ILLEGAL();
                r = (f6 == 0x10) ? (u64)(sa >> sh) : (a >> sh);
            }
        } else {
            switch (f3) {
            case 0x0:
                r = a + (u64)imm;
                break;
            case 0x2:
                r = (u64)(sa < imm);
                break; /* slti  (signed)   */
            case 0x3:
                r = (u64)(a < (u64)imm);
                break; /* sltiu (unsigned) */
            case 0x4:
                r = a ^ (u64)imm;
                break;
            case 0x6:
                r = a | (u64)imm;
                break;
            case 0x7:
                r = a & (u64)imm;
                break;
            default:
                TRAP_ILLEGAL();
            }
        }
        cpu_wr(c, rd, r);
        break;
    }

    /* ------------------------------------------------------ OP-IMM-32 */
    case 0x1B: {
        s64 imm = imm_i(insn);
        s32 w = (s32)a;
        s32 r = 0;
        switch (f3) {
        case 0x0:
            /* addiw wraps: do it in u32 so the host cannot overflow. */
            r = (s32)((u32)w + (u32)(s32)imm);
            break;
        case 0x1: {
            u32 sh = (u32)(insn >> 20) & 0x1F;
            if (f7 != 0x00)
                TRAP_ILLEGAL();
            r = (s32)((u32)w << sh);
            break;
        }
        case 0x5: {
            u32 sh = (u32)(insn >> 20) & 0x1F;
            if (f7 != 0x00 && f7 != 0x20)
                TRAP_ILLEGAL();
            r = (f7 == 0x20) ? (w >> sh) : (s32)((u32)w >> sh);
            break;
        }
        default:
            TRAP_ILLEGAL();
        }
        cpu_wr(c, rd, (u64)(s64)r);
        break;
    }

    /* ------------------------------------------------------------- OP */
    case 0x33: {
        u64 r = 0;
        if (f7 == 0x01) { /* M extension */
            switch (f3) {
            case 0x0:
                /* mul keeps the low 64 bits, which is the same for a signed
                 * and an unsigned product -- and only the unsigned one is
                 * defined when it wraps. */
                r = a * b;
                break;
            case 0x1:
                r = (u64)(((__int128)sa * (__int128)sb) >> 64);
                break;
            case 0x2: {
                /* MULHSU: signed rs1 times *unsigned* rs2.  Converting the u64
                 * to __int128 preserves its value, which is exactly the
                 * zero-extension the instruction requires. */
                __int128 p = (__int128)sa * (__int128)(u64)b;
                r = (u64)(p >> 64);
                break;
            }
            case 0x3: {
                unsigned __int128 p = (unsigned __int128)a * b;
                r = (u64)(p >> 64);
                break;
            }
            /* DIV/REM must be guarded *before* the host division: x86 raises
             * SIGFPE on INT64_MIN / -1, and the spec defines that case. */
            case 0x4:
                if (b == 0)
                    r = UINT64_MAX;
                else if (sa == INT64_MIN && sb == -1)
                    r = (u64)INT64_MIN;
                else
                    r = (u64)(sa / sb);
                break;
            case 0x5:
                r = (b == 0) ? UINT64_MAX : a / b;
                break;
            case 0x6:
                if (b == 0)
                    r = a;
                else if (sa == INT64_MIN && sb == -1)
                    r = 0;
                else
                    r = (u64)(sa % sb);
                break;
            case 0x7:
                r = (b == 0) ? a : a % b;
                break;
            default:
                TRAP_ILLEGAL();
            }
        } else {
            /* funct7 must be 0x00 or 0x20; anything else is reserved. */
            if (f7 != 0x00 && f7 != 0x20)
                TRAP_ILLEGAL();
            /* Shifts and compares only exist with funct7 = 0x00. */
            if (f7 == 0x20 && f3 != 0x0 && f3 != 0x5)
                TRAP_ILLEGAL();
            switch (f3) {
            case 0x0:
                r = (f7 == 0x20) ? (a - b) : (a + b);
                break;
            case 0x1:
                r = a << (b & 63);
                break;
            case 0x2:
                r = (u64)(sa < sb);
                break;
            case 0x3:
                r = (u64)(a < b);
                break;
            case 0x4:
                r = a ^ b;
                break;
            case 0x5:
                if (f7 != 0x00 && f7 != 0x20)
                    TRAP_ILLEGAL();
                r = (f7 == 0x20) ? (u64)(sa >> (b & 63)) : (a >> (b & 63));
                break;
            case 0x6:
                r = a | b;
                break;
            case 0x7:
                r = a & b;
                break;
            }
        }
        cpu_wr(c, rd, r);
        break;
    }

    /* ----------------------------------------------------------- OP-32 */
    case 0x3B: {
        s32 w1 = (s32)a, w2 = (s32)b;
        s32 r = 0;
        u32 sh = (u32)(b & 31);
        if (f7 == 0x01) { /* M-extension word ops */
            switch (f3) {
            case 0x0:
                r = (s32)((u32)w1 * (u32)w2);
                break;
            case 0x4:
                r = (w2 == 0) ? -1 : (w1 == INT32_MIN && w2 == -1) ? INT32_MIN : (s32)(w1 / w2);
                break;
            case 0x5:
                r = (w2 == 0) ? -1 : (s32)((u32)w1 / (u32)w2);
                break;
            case 0x6:
                r = (w2 == 0) ? w1 : (w1 == INT32_MIN && w2 == -1) ? 0 : (s32)(w1 % w2);
                break;
            case 0x7:
                r = (w2 == 0) ? w1 : (s32)((u32)w1 % (u32)w2);
                break;
            default:
                TRAP_ILLEGAL();
            }
        } else {
            if (f7 != 0x00 && f7 != 0x20)
                TRAP_ILLEGAL();
            if (f7 == 0x20 && f3 != 0x0 && f3 != 0x5)
                TRAP_ILLEGAL();
            switch (f3) {
            case 0x0:
                r = (f7 == 0x20) ? (s32)((u32)w1 - (u32)w2) : (s32)((u32)w1 + (u32)w2);
                break;
            case 0x1:
                if (f7 != 0x00)
                    TRAP_ILLEGAL();
                r = (s32)((u32)w1 << sh);
                break;
            case 0x5:
                if (f7 != 0x00 && f7 != 0x20)
                    TRAP_ILLEGAL();
                r = (f7 == 0x20) ? (w1 >> sh) : (s32)((u32)w1 >> sh);
                break;
            default:
                TRAP_ILLEGAL();
            }
        }
        cpu_wr(c, rd, (u64)(s64)r);
        break;
    }

    /* ------------------------------------------------- MISC-MEM/AMO */
    case 0x0F: /* fence / fence.i -- ordering is a no-op in a single hart */
        break;

    case 0x2F: {
        u32 size = (f3 == 0x2) ? 4 : (f3 == 0x3) ? 8 : 0;
        if (!size)
            TRAP_ILLEGAL();
        bool ok = true;
        do_amo(c, insn, size, res, &ok);
        /* do_amo() returns before the shared epilogue, so advance pc here. */
        if (ok)
            c->pc = c->pc + (c->last_insn_len ? c->last_insn_len : 4);
        return ok;
    }

    /* ------------------------------------------------------ FP opcodes */
    /* LOAD-FP, STORE-FP, OP-FP and the four fused-multiply-add opcodes. */
    case 0x07:
    case 0x27:
    case 0x53:
    case 0x43:
    case 0x47:
    case 0x4B:
    case 0x4F: {
        bool handled = fp_exec(c, insn, res);
        /* fp_exec() also returns before the shared epilogue. */
        if (handled)
            c->pc = c->pc + (c->last_insn_len ? c->last_insn_len : 4);
        return handled;
    }

    /* --------------------------------------------------------- SYSTEM */
    case 0x73: {
        if (f3 == 0) {
            u32 which = insn >> 20;
            switch (which) {
            case 0x000: /* ecall */
                c->n_ecalls++;
                cpu_trap(c,
                         c->priv == PRV_U ? EXC_ECALL_U
                                          : (c->priv == PRV_S ? EXC_ECALL_S : EXC_ECALL_M),
                         0, false);
                *res = (c->priv == PRV_M) ? STEP_ECALL_M : STEP_TRAP;
                /* cpu_trap moved us to M-mode; report the origin instead. */
                *res = (c->last_from_priv == PRV_M) ? STEP_ECALL_M : STEP_TRAP;
                return false;
            case 0x001: /* ebreak */
                cpu_trap(c, EXC_BREAKPOINT, c->pc, false);
                *res = STEP_TRAP;
                return false;
            case 0x102: /* sret */
                if (c->priv < PRV_S)
                    TRAP_ILLEGAL();
                if (c->priv == PRV_S && (c->csr[CSR_MSTATUS] & MSTATUS_TSR))
                    TRAP_ILLEGAL();
                cpu_sret(c);
                npc = c->pc; /* cpu_sret() already wrote sepc into pc */
                taken = true;
                break;
            case 0x302: /* mret */
                if (c->priv < PRV_M)
                    TRAP_ILLEGAL();
                cpu_mret(c);
                npc = c->pc; /* cpu_mret() already wrote mepc into pc */
                taken = true;
                break;
            case 0x105: /* wfi */
                *res = STEP_WFI;
                return false;
            default:
                if ((which & 0xFE0) == 0x120) { /* sfence.vma */
                    if (c->priv < PRV_S)
                        TRAP_ILLEGAL();
                    if (c->priv == PRV_S && (c->csr[CSR_MSTATUS] & MSTATUS_TVM))
                        TRAP_ILLEGAL();
                    if (rs1 == 0)
                        mmu_flush(c->mmu);
                    else
                        mmu_flush_page(c->mmu, a);
                    break;
                }
                TRAP_ILLEGAL();
            }
            break;
        }
        /* CSRRW / CSRRS / CSRRC / immediate variants */
        u32 caddr = (insn >> 20) & 0xFFF;
        bool imm_form = (f3 & 4) != 0;
        u64 src = imm_form ? (u64)rs1 : a;
        bool illegal = false;
        u64 old = csr_read(c, caddr, &illegal);
        if (illegal)
            TRAP_ILLEGAL();
        /* Privilege check: S-mode may not touch M-level CSRs. */
        u32 csr_priv = (caddr >> 8) & 3;
        if (c->priv < csr_priv) {
            cpu_trap(c, EXC_ILLEGAL_INST, insn, false);
            *res = STEP_TRAP;
            return false;
        }
        u64 nv = old;
        bool do_write = true;
        switch (f3 & 3) {
        case 1:
            nv = src;
            break; /* csrrw  */
        case 2:
            nv = old | src;
            break; /* csrrs  */
        case 3:
            nv = old & ~src;
            break; /* csrrc  */
        default:
            TRAP_ILLEGAL();
        }
        /* csrrs/csrrc with a zero source must not write, so that reading a CSR
         * with side effects stays side-effect free. */
        if ((f3 & 3) != 1 && src == 0)
            do_write = false;
        /* csrrw with rd=x0 must not read. */
        if ((f3 & 3) == 1 && rd == 0)
            old = 0;
        if (do_write) {
            bool wr_illegal = false;
            csr_write(c, caddr, nv, &wr_illegal);
            if (wr_illegal)
                TRAP_ILLEGAL();
        }
        cpu_wr(c, rd, old);
        break;
    }

    default:
        TRAP_ILLEGAL();
    }

    c->pc = taken ? npc : (c->pc + (c->last_insn_len ? c->last_insn_len : 4));
    return true;
}

/* ------------------------------------------------------- A extension AMO */

static void do_amo(cpu *c, u32 insn, u32 size, step_result *res, bool *ok) {
    u32 rd = RD(insn), rs1 = RS1(insn), rs2 = RS2(insn);
    u32 funct5 = insn >> 27;
    u64 addr = cpu_rd(c, rs1);
    u64 b = cpu_rd(c, rs2);
    u32 cause = 0;
    *ok = true;

    if (funct5 == 0x02) { /* LR */
        u64 v = 0;
        if (!cpu_mem_load(c, addr, size, &v, &cause)) {
            cpu_trap(c, cause, addr, false);
            *res = STEP_TRAP;
            *ok = false;
            return;
        }
        c->rsrv_addr = addr;
        c->rsrv_valid = true;
        cpu_wr(c, rd, (size == 4) ? (u64)(s64)(s32)v : v);
        return;
    }
    if (funct5 == 0x03) { /* SC */
        if (!c->rsrv_valid || c->rsrv_addr != addr) {
            cpu_wr(c, rd, 1); /* fail */
            return;
        }
        if (!cpu_mem_store(c, addr, size, b, &cause)) {
            c->rsrv_valid = false;
            cpu_trap(c, cause, addr, false);
            *res = STEP_TRAP;
            *ok = false;
            return;
        }
        c->rsrv_valid = false;
        cpu_wr(c, rd, 0); /* success */
        return;
    }

    u64 old = 0;
    if (!cpu_mem_load(c, addr, size, &old, &cause)) {
        cpu_trap(c, cause, addr, false);
        *res = STEP_TRAP;
        *ok = false;
        return;
    }
    /* For the .W forms both operands are the low 32 bits of the register or
     * memory word.  Signed compares need them sign-extended, unsigned compares
     * zero-extended -- mixing the two is what makes amominu/amomaxu wrong. */
    u64 uo = (size == 4) ? (u64)(u32)old : old;
    u64 ub = (size == 4) ? (u64)(u32)b : b;
    if (size == 4)
        old = (u64)(s32)(u32)old;
    s64 so = (s64)old, sb = (size == 4) ? (s64)(s32)(u32)b : (s64)b;
    u64 r = 0;
    switch (funct5) {
    case 0x01:
        r = b;
        break; /* amoswap */
    case 0x00:
        r = old + b;
        break; /* amoadd  */
    case 0x04:
        r = old ^ b;
        break; /* amoxor  */
    case 0x0C:
        r = old & b;
        break; /* amoand  */
    case 0x08:
        r = old | b;
        break; /* amoor   */
    case 0x10:
        r = (u64)RVM_MIN(so, sb);
        break; /* amomin  */
    case 0x14:
        r = (u64)RVM_MAX(so, sb);
        break; /* amomax  */
    case 0x18:
        r = RVM_MIN(uo, ub);
        break; /* amominu */
    case 0x1C:
        r = RVM_MAX(uo, ub);
        break; /* amomaxu */
    default:
        cpu_trap(c, EXC_ILLEGAL_INST, insn, false);
        *res = STEP_TRAP;
        *ok = false;
        return;
    }
    if (size == 4)
        r = (u64)(s64)(s32)r;
    c->rsrv_valid = false;
    if (!cpu_mem_store(c, addr, size, r, &cause)) {
        cpu_trap(c, cause, addr, false);
        *res = STEP_TRAP;
        *ok = false;
        return;
    }
    cpu_wr(c, rd, old);
}

/* ------------------------------------------------------------- cpu_step */

step_result cpu_step(cpu *c) {
    if (c->halted)
        return STEP_SHUTDOWN;

    c->cycles++;

    /* Interrupts are checked before the fetch, as the spec requires. */
    u64 irq = cpu_pending_interrupt(c);
    if (irq) {
        cpu_trap(c, irq & ~(1ULL << 63), 0, true);
        return STEP_TRAP;
    }

    u32 insn = 0, len = 0, cause = 0;
    if (!fetch(c, &insn, &len, &cause)) {
        cpu_trap(c, cause, c->pc, false);
        return STEP_TRAP;
    }

    void *ud = NULL;
    rvm_trace_fn tf = rvm_trace_get(&ud);
    if (tf)
        tf(ud, c->pc, insn, len);

    if (len == 2) {
        bool illegal = false;
        u32 exp = c_expand((u16)insn, &illegal);
        if (illegal || exp == 0) {
            cpu_trap(c, EXC_ILLEGAL_INST, insn, false);
            return STEP_TRAP;
        }
        insn = exp;
    }

    c->last_insn_len = len;
    step_result res = STEP_OK;
    bool fine = cpu_exec32(c, insn, &res);
    if (fine) {
        c->instret++;
        return STEP_OK;
    }
    /* Traps still retire as an executed instruction for counting purposes,
     * but WFI/ECALL_M/SHUTDOWN must be surfaced to the run loop. */
    if (res == STEP_TRAP)
        c->instret++;
    return res;
}
