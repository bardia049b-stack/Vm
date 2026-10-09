/*
 * fp.c -- F (single) and D (double) extensions.
 *
 * Host float/double are used directly and IEEE exception flags are harvested
 * from <fenv.h>, which is part of libc -- no softfloat dependency, keeping the
 * "libc + libm only" promise from PLAN.
 *
 * Documented limitation: rounding mode RMM (rm=4) has no C equivalent and is
 * executed as RNE.  No mainstream toolchain or the Linux kernel emits RMM, so
 * it cannot affect a Debian boot; it is called out here rather than silently
 * approximated.
 *
 * SPDX-License-Identifier: MIT
 */
#include "cpu.h"

#include <fenv.h>
#include <math.h>

#define FMT_S 0
#define FMT_D 1

#define FF_NX 0x01u
#define FF_UF 0x02u
#define FF_OF 0x04u
#define FF_DZ 0x08u
#define FF_NV 0x10u

/* OP-FP operation groups: funct7 = (op5 << 2) | fmt */
#define FP_ADD    0x00
#define FP_SUB    0x01
#define FP_MUL    0x02
#define FP_DIV    0x03
#define FP_SGNJ   0x04
#define FP_MINMAX 0x05
#define FP_CVTSD  0x08 /* fcvt.s.d / fcvt.d.s */
#define FP_CMP    0x09 /* feq / flt / fle       */
#define FP_SQRT   0x0B
#define FP_F2I    0x18
#define FP_I2F    0x1A
#define FP_MVXW   0x1C /* fmv.x.w / fmv.x.d / fclass */
#define FP_MVWX   0x1E /* fmv.w.x / fmv.d.x */

/* ---------------------------------------------------------------- boxing */

static inline u64 box32(u32 bits) {
    return 0xFFFFFFFF00000000ULL | bits;
}
static inline u32 bits32(float f) {
    u32 b;
    memcpy(&b, &f, 4);
    return b;
}
static inline float from32(u32 b) {
    float f;
    memcpy(&f, &b, 4);
    return f;
}
static inline u64 bits64(double d) {
    u64 b;
    memcpy(&b, &d, 8);
    return b;
}
static inline double from64(u64 b) {
    double d;
    memcpy(&d, &b, 8);
    return d;
}
/* An improperly NaN-boxed single is read as the canonical quiet NaN. */
static inline u32 unbox(const cpu *c, u32 r) {
    u64 v = c->f[r & 31];
    return ((v >> 32) == 0xFFFFFFFFu) ? (u32)v : 0x7FC00000u;
}

static void wr_f(cpu *c, u32 r, u64 v) {
    c->f[r & 31] = v;
    /* Any FP register write marks the FP context Dirty in mstatus.FS. */
    c->csr[CSR_MSTATUS] = (c->csr[CSR_MSTATUS] & ~MSTATUS_FS) | (3ULL << MSTATUS_FS_SHIFT);
}

/* ------------------------------------------------------- rounding / flags */

static void set_rm(cpu *c, u32 rm) {
    if (rm == 7)
        rm = (u32)((c->csr[CSR_FCSR] >> 5) & 7);
    switch (rm) {
    case 0:
        fesetround(FE_TONEAREST);
        break;
    case 1:
        fesetround(FE_TOWARDZERO);
        break;
    case 2:
        fesetround(FE_DOWNWARD);
        break;
    case 3:
        fesetround(FE_UPWARD);
        break;
    default:
        fesetround(FE_TONEAREST);
        break; /* RMM -> RNE, see header */
    }
}

static u32 harvest(void) {
    u32 f = 0;
    if (fetestexcept(FE_INVALID))
        f |= FF_NV;
    if (fetestexcept(FE_DIVBYZERO))
        f |= FF_DZ;
    if (fetestexcept(FE_OVERFLOW))
        f |= FF_OF;
    if (fetestexcept(FE_UNDERFLOW))
        f |= FF_UF;
    if (fetestexcept(FE_INEXACT))
        f |= FF_NX;
    return f;
}

/* ----------------------------------------------------------- conversions */

/*
 * FP -> integer, with the saturating behaviour the spec mandates: NaN or an
 * out-of-range result raises NV and yields the target type's extreme value.
 */
static u64 fcvt_to_int(double v, u32 width, bool is_signed, u32 *fflags) {
    double lo, hi;
    u64 invalid;
    if (is_signed) {
        lo = -ldexp(1.0, (int)width - 1);
        hi = ldexp(1.0, (int)width - 1); /* exclusive upper bound */
        invalid = (width >= 64) ? (u64)INT64_MIN : (u64)(s64)(-(s64)(1LL << (width - 1)));
    } else {
        lo = 0.0;
        hi = ldexp(1.0, (int)width);
        invalid = (width >= 64) ? UINT64_MAX : ((1ULL << width) - 1);
    }

    if (isnan(v)) {
        *fflags |= FF_NV;
        return invalid;
    }
    double r = rint(v); /* honours the rounding mode set by set_rm() */
    if (r < lo || r >= hi) {
        /* Out of range: the spec saturates to the nearest representable value
         * and raises NV.  NaN keeps the dedicated `invalid` pattern above. */
        *fflags |= FF_NV;
        if (!is_signed)
            return (r < 0.0) ? 0 : ((width >= 64) ? UINT64_MAX : UINT32_MAX);
        if (width >= 64)
            return (u64)((r > 0.0) ? INT64_MAX : INT64_MIN);
        return (u64)(s64)(s32)((r > 0.0) ? INT32_MAX : INT32_MIN);
    }
    if (r != v)
        *fflags |= FF_NX;

    if (is_signed) {
        if (width == 32)
            return (u64)(s64)(s32)r;
        return (u64)(s64)r;
    }
    if (width == 32)
        return (u64)(u32)r;
    return (u64)r;
}

/* fclass: bit 0:-inf 1:-normal 2:-sub 3:-0 4:+0 5:+sub 6:+normal 7:+inf
 *         8:sNaN 9:qNaN */
static u64 fclass(u64 b, bool single) {
    u32 exp_w = single ? 8 : 11;
    u32 sig_w = single ? 23 : 52;
    u64 sign = (b >> (single ? 31 : 63)) & 1;
    u64 exp = (b >> sig_w) & ((1ULL << exp_w) - 1);
    u64 sig = b & ((1ULL << sig_w) - 1);
    u64 emax = (1ULL << exp_w) - 1;
    bool msb = (bool)((sig >> (sig_w - 1)) & 1);

    if (exp == emax && sig != 0)
        return msb ? (1ULL << 9) : (1ULL << 8);
    if (exp == emax)
        return sign ? (1ULL << 0) : (1ULL << 7);
    if (exp == 0 && sig == 0)
        return sign ? (1ULL << 3) : (1ULL << 4);
    if (exp == 0)
        return sign ? (1ULL << 2) : (1ULL << 5);
    return sign ? (1ULL << 1) : (1ULL << 6);
}

/* --------------------------------------------------------------- execute */

bool fp_exec(cpu *c, u32 insn, step_result *res) {
    u32 op = insn & 0x7F;
    u32 rd = (insn >> 7) & 0x1F, rs1 = (insn >> 15) & 0x1F, rs2 = (insn >> 20) & 0x1F;
    u32 f3 = (insn >> 12) & 7, f7 = (insn >> 25) & 0x7F;
    u32 fmt = f7 & 3;
    u32 extra = 0; /* flags raised outside of fenv */

#define FP_ILLEGAL()                                                                               \
    do {                                                                                           \
        cpu_trap(c, EXC_ILLEGAL_INST, insn, false);                                                \
        *res = STEP_TRAP;                                                                          \
        return false;                                                                              \
    } while (0)

    /* FP state must not be Off. */
    if (((c->csr[CSR_MSTATUS] & MSTATUS_FS) >> MSTATUS_FS_SHIFT) == 0)
        FP_ILLEGAL();

    /* ------------------------------------------------- LOAD-FP / STORE-FP */
    if (op == 0x07 || op == 0x27) {
        u64 addr;
        if (op == 0x07) {
            addr = cpu_rd(c, rs1) + (u64)(s64)(s32)(insn >> 20);
        } else {
            u32 u = (((insn >> 25) & 0x7F) << 5) | ((insn >> 7) & 0x1F);
            addr = cpu_rd(c, rs1) + (u64)(s64)((s32)(u << 20) >> 20);
        }
        u32 cause = 0;
        if (op == 0x07) {
            u64 v = 0;
            if (f3 == 2) {
                if (!cpu_mem_load(c, addr, 4, &v, &cause)) {
                    cpu_trap(c, cause, addr, false);
                    *res = STEP_TRAP;
                    return false;
                }
                wr_f(c, rd, box32((u32)v));
            } else if (f3 == 3) {
                if (!cpu_mem_load(c, addr, 8, &v, &cause)) {
                    cpu_trap(c, cause, addr, false);
                    *res = STEP_TRAP;
                    return false;
                }
                wr_f(c, rd, v);
            } else {
                FP_ILLEGAL();
            }
        } else {
            if (f3 == 2) {
                if (!cpu_mem_store(c, addr, 4, unbox(c, rs2), &cause)) {
                    cpu_trap(c, cause, addr, false);
                    *res = STEP_TRAP;
                    return false;
                }
            } else if (f3 == 3) {
                if (!cpu_mem_store(c, addr, 8, c->f[rs2 & 31], &cause)) {
                    cpu_trap(c, cause, addr, false);
                    *res = STEP_TRAP;
                    return false;
                }
            } else {
                FP_ILLEGAL();
            }
        }
        return true;
    }

    /* --------------------------------------------- fused multiply-add */
    if (op == 0x43 || op == 0x47 || op == 0x4B || op == 0x4F) {
        if (fmt > FMT_D)
            FP_ILLEGAL();
        u32 rs3 = (insn >> 27) & 0x1F;
        set_rm(c, f3);
        feclearexcept(FE_ALL_EXCEPT);
        if (fmt == FMT_S) {
            float a = from32(unbox(c, rs1)), b = from32(unbox(c, rs2)), cc = from32(unbox(c, rs3));
            float r;
            switch (op) {
            case 0x43:
                r = fmaf(a, b, cc);
                break; /* fmadd  */
            case 0x47:
                r = fmaf(a, b, -cc);
                break; /* fmsub  */
            case 0x4B:
                r = fmaf(-a, b, cc);
                break; /* fnmsub */
            default:
                r = fmaf(-a, b, -cc);
                break; /* fnmadd */
            }
            wr_f(c, rd, box32(bits32(r)));
        } else {
            double a = from64(c->f[rs1]), b = from64(c->f[rs2]), cc = from64(c->f[rs3]);
            double r;
            switch (op) {
            case 0x43:
                r = fma(a, b, cc);
                break;
            case 0x47:
                r = fma(a, b, -cc);
                break;
            case 0x4B:
                r = fma(-a, b, cc);
                break;
            default:
                r = fma(-a, b, -cc);
                break;
            }
            wr_f(c, rd, bits64(r));
        }
        c->csr[CSR_FCSR] |= harvest();
        return true;
    }

    if (op != 0x53)
        FP_ILLEGAL();
    if (fmt > FMT_D)
        FP_ILLEGAL();

    feclearexcept(FE_ALL_EXCEPT);
    if (f3 <= 4 || f3 == 7)
        set_rm(c, f3); /* arithmetic rounding mode */

    switch (f7 >> 2) {
    case FP_ADD:
    case FP_SUB:
    case FP_MUL:
    case FP_DIV:
        if (fmt == FMT_S) {
            float a = from32(unbox(c, rs1)), b = from32(unbox(c, rs2)), r;
            switch (f7 >> 2) {
            case FP_ADD:
                r = a + b;
                break;
            case FP_SUB:
                r = a - b;
                break;
            case FP_MUL:
                r = a * b;
                break;
            default:
                r = a / b;
                break;
            }
            wr_f(c, rd, box32(bits32(r)));
        } else {
            double a = from64(c->f[rs1]), b = from64(c->f[rs2]), r;
            switch (f7 >> 2) {
            case FP_ADD:
                r = a + b;
                break;
            case FP_SUB:
                r = a - b;
                break;
            case FP_MUL:
                r = a * b;
                break;
            default:
                r = a / b;
                break;
            }
            wr_f(c, rd, bits64(r));
        }
        break;

    case FP_SGNJ:
        if (f3 > 2)
            FP_ILLEGAL();
        if (fmt == FMT_S) {
            u32 a = unbox(c, rs1), b = unbox(c, rs2), sign;
            switch (f3) {
            case 0:
                sign = b >> 31;
                break; /* fsgnj  */
            case 1:
                sign = !(b >> 31);
                break; /* fsgnjn */
            default:
                sign = (a >> 31) ^ (b >> 31);
                break; /* fsgnjx */
            }
            wr_f(c, rd, box32((sign << 31) | (a & 0x7FFFFFFFu)));
        } else {
            u64 a = c->f[rs1], b = c->f[rs2], sign;
            switch (f3) {
            case 0:
                sign = b >> 63;
                break;
            case 1:
                sign = !(b >> 63);
                break;
            default:
                sign = (a >> 63) ^ (b >> 63);
                break;
            }
            wr_f(c, rd, (sign << 63) | (a & 0x7FFFFFFFFFFFFFFFULL));
        }
        break;

    case FP_MINMAX:
        if (f3 > 1)
            FP_ILLEGAL();
        if (fmt == FMT_S) {
            float a = from32(unbox(c, rs1)), b = from32(unbox(c, rs2));
            float r = (f3 == 0) ? fminf(a, b) : fmaxf(a, b);
            wr_f(c, rd, box32(bits32(r)));
        } else {
            double a = from64(c->f[rs1]), b = from64(c->f[rs2]);
            double r = (f3 == 0) ? fmin(a, b) : fmax(a, b);
            wr_f(c, rd, bits64(r));
        }
        break;

    case FP_CVTSD:
        if (fmt == FMT_S) {
            if (rs2 != 1)
                FP_ILLEGAL();
            wr_f(c, rd, box32(bits32((float)from64(c->f[rs1])))); /* fcvt.s.d */
        } else {
            if (rs2 != 0)
                FP_ILLEGAL();
            wr_f(c, rd, bits64((double)from32(unbox(c, rs1)))); /* fcvt.d.s */
        }
        break;

    case FP_SQRT:
        if (rs2 != 0)
            FP_ILLEGAL();
        if (fmt == FMT_S)
            wr_f(c, rd, box32(bits32(sqrtf(from32(unbox(c, rs1))))));
        else
            wr_f(c, rd, bits64(sqrt(from64(c->f[rs1]))));
        break;

    case FP_I2F: {
        u64 src = cpu_rd(c, rs1);
        double dv;
        switch (rs2) {
        case 0:
            dv = (double)(s32)src;
            break;
        case 1:
            dv = (double)(u32)src;
            break;
        case 2:
            dv = (double)(s64)src;
            break;
        case 3:
            dv = (double)(u64)src;
            break;
        default:
            FP_ILLEGAL();
        }
        if (fmt == FMT_S)
            wr_f(c, rd, box32(bits32((float)dv)));
        else
            wr_f(c, rd, bits64(dv));
        break;
    }

    case FP_F2I: {
        u32 w = (rs2 >= 2) ? 64 : 32;
        bool sg = (rs2 & 1) == 0;
        double dv = (fmt == FMT_S) ? (double)from32(unbox(c, rs1)) : from64(c->f[rs1]);
        cpu_wr(c, rd, fcvt_to_int(dv, w, sg, &extra));
        break;
    }

    case FP_CMP: {
        /* feq (f3=2), flt (f3=1), fle (f3=0).  flt/fle raise NV on any NaN;
         * feq only raises on a signalling NaN, which we cannot distinguish
         * cheaply, so it stays quiet like the reference model does for qNaN. */
        if (f3 > 2)
            FP_ILLEGAL();
        u64 r = 0;
        if (fmt == FMT_S) {
            float a = from32(unbox(c, rs1)), b = from32(unbox(c, rs2));
            if (f3 != 2 && (a != a || b != b))
                extra |= 0x10; /* NV */
            r = (f3 == 2) ? (u64)(a == b) : (f3 == 1) ? (u64)(a < b) : (u64)(a <= b);
        } else {
            double a = from64(c->f[rs1]), b = from64(c->f[rs2]);
            if (f3 != 2 && (a != a || b != b))
                extra |= 0x10; /* NV */
            r = (f3 == 2) ? (u64)(a == b) : (f3 == 1) ? (u64)(a < b) : (u64)(a <= b);
        }
        cpu_wr(c, rd, r);
        break;
    }

    case FP_MVXW:
        if (f3 == 1) { /* fclass */
            if (rs2 != 0)
                FP_ILLEGAL();
            cpu_wr(c, rd, (fmt == FMT_S) ? fclass(unbox(c, rs1), true) : fclass(c->f[rs1], false));
        } else if (f3 == 0) { /* fmv.x.w / fmv.x.d */
            if (rs2 != 0)
                FP_ILLEGAL();
            if (fmt == FMT_S)
                cpu_wr(c, rd, (u64)(s64)(s32)unbox(c, rs1));
            else
                cpu_wr(c, rd, c->f[rs1]);
        } else {
            FP_ILLEGAL();
        }
        break;

    case FP_MVWX:
        if (f3 != 0 || rs2 != 0)
            FP_ILLEGAL();
        if (fmt == FMT_S)
            wr_f(c, rd, box32((u32)cpu_rd(c, rs1)));
        else
            wr_f(c, rd, cpu_rd(c, rs1));
        break;

    default:
        FP_ILLEGAL();
    }

    c->csr[CSR_FCSR] |= (harvest() | extra);
    return true;

#undef FP_ILLEGAL
}
