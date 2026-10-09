/*
 * harness.h -- a miniature machine for tests: bus + MMU + hart, plus helpers
 *              to assemble guest code with real RISC-V encodings.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RVM_HARNESS_H
#define RVM_HARNESS_H

#include "../../src/bus/bus.h"
#include "../../src/cpu/cpu.h"
#include "../../src/devices/clint.h"
#include "../../src/devices/plic.h"
#include "../../src/devices/uart.h"
#include "../../src/devices/virtio.h"
#include "../../src/devices/virtio_blk.h"
#include "../../src/mmu/mmu.h"
#include "../../src/rvm.h"

#define TH_CODE_ADDR (RVM_RAM_BASE + 0x1000ULL)
#define TH_DATA_ADDR (RVM_RAM_BASE + 0x100000ULL)

typedef struct th {
    bus bus;
    mmu mmu;
    cpu cpu;
    clint clint;
    plic plic;
    uart uart;
    u64 code;   /* next address th_emit writes to */
    u64 steps;  /* instructions executed by th_run */
    u8 out[8192];
    u32 outlen;
} th;

rvm_err th_init(th *t);
void th_free(th *t);

/* Append one instruction word and return its address. */
u64 th_emit(th *t, u32 insn);
/* Append raw bytes to the code stream (used for compressed tests). */
u64 th_emit16(th *t, u16 insn);

/* Execute exactly n instructions from the current pc. */
void th_run(th *t, u32 n);

/*
 * Execute until pc reaches the end of the emitted stream (or 8192 steps).
 * Straight-line tests use this so helper instructions emitted by th_li()
 * cannot throw off an explicit step count.
 */
void th_run_all(th *t);

/*
 * Emit guest instructions that materialise the 32-bit value `v` into `rd`,
 * correctly handling the sign-extension trap for values with bit 31 set
 * (e.g. any address in our RAM window at 0x80000000).
 * Emits between 1 and 6 instructions.
 */
void th_li(th *t, u32 rd, u32 v);

/* Memory peek/poke in guest RAM (physical). */
void th_poke64(th *t, u64 addr, u64 v);
u64 th_peek64(th *t, u64 addr);
void th_poke32(th *t, u64 addr, u32 v);
u32 th_peek32(th *t, u64 addr);

/* ------------------------------------------------------- encoders */

#define OP_LUI 0x37
#define OP_AUIPC 0x17
#define OP_JAL 0x6F
#define OP_JALR 0x67
#define OP_BRANCH 0x63
#define OP_LOAD 0x03
#define OP_STORE 0x23
#define OP_OPIMM 0x13
#define OP_OP 0x33
#define OP_OPIMM32 0x1B
#define OP_OP32 0x3B
#define OP_MISC_MEM 0x0F
#define OP_AMO 0x2F
#define OP_SYSTEM 0x73
#define OP_LOADFP 0x07
#define OP_STOREFP 0x27
#define OP_FMADD 0x43
#define OP_FMSUB 0x47
#define OP_FNMSUB 0x4B
#define OP_FNMADD 0x4F
#define OP_OPFP 0x53

/* Register nicknames */
#define X_ZERO 0
#define X_RA 1
#define X_SP 2
#define X_GP 3
#define X_TP 4
#define X_T0 5
#define X_T1 6
#define X_T2 7
#define X_S0 8
#define X_S1 9
#define X_A0 10
#define X_A1 11
#define X_A2 12
#define X_A3 13
#define X_A4 14
#define X_A5 15
#define X_A7 17

static inline u32 ENC_R(u32 op, u32 rd, u32 f3, u32 rs1, u32 rs2, u32 f7)
{
    return (f7 << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}
static inline u32 ENC_I(u32 op, u32 rd, u32 f3, u32 rs1, s32 imm)
{
    return (((u32)imm & 0xFFF) << 20) | (rs1 << 15) | (f3 << 12) | (rd << 7) | op;
}
static inline u32 ENC_S(u32 op, u32 f3, u32 rs1, u32 rs2, s32 imm)
{
    u32 u = (u32)imm & 0xFFF;
    return ((u >> 5) << 25) | (rs2 << 20) | (rs1 << 15) | (f3 << 12) | ((u & 0x1F) << 7) | op;
}
static inline u32 ENC_B(u32 op, u32 f3, u32 rs1, u32 rs2, s32 imm)
{
    u32 u = (u32)imm;
    return (((u >> 12) & 1) << 31) | (((u >> 5) & 0x3F) << 25) | (rs2 << 20) | (rs1 << 15) |
           (f3 << 12) | (((u >> 1) & 0xF) << 8) | (((u >> 11) & 1) << 7) | op;
}
static inline u32 ENC_U(u32 op, u32 rd, u32 imm) { return (imm & 0xFFFFF000u) | (rd << 7) | op; }
static inline u32 ENC_J(u32 op, u32 rd, s32 imm)
{
    u32 u = (u32)imm;
    return (((u >> 20) & 1) << 31) | (((u >> 1) & 0x3FF) << 21) | (((u >> 11) & 1) << 20) |
           (((u >> 12) & 0xFF) << 12) | (rd << 7) | op;
}

/* Readable mnemonics for the most common instructions */
#define LUI(rd, imm) ENC_U(OP_LUI, rd, imm)
#define AUIPC(rd, imm) ENC_U(OP_AUIPC, rd, imm)
#define ADD(rd, rs1, rs2) ENC_R(OP_OP, rd, 0, rs1, rs2, 0x00)
#define SUB(rd, rs1, rs2) ENC_R(OP_OP, rd, 0, rs1, rs2, 0x20)
#define AND(rd, rs1, rs2) ENC_R(OP_OP, rd, 7, rs1, rs2, 0x00)
#define OR(rd, rs1, rs2) ENC_R(OP_OP, rd, 6, rs1, rs2, 0x00)
#define XOR(rd, rs1, rs2) ENC_R(OP_OP, rd, 4, rs1, rs2, 0x00)
#define SLL(rd, rs1, rs2) ENC_R(OP_OP, rd, 1, rs1, rs2, 0x00)
#define SRL(rd, rs1, rs2) ENC_R(OP_OP, rd, 5, rs1, rs2, 0x00)
#define SRA(rd, rs1, rs2) ENC_R(OP_OP, rd, 5, rs1, rs2, 0x20)
#define SLT(rd, rs1, rs2) ENC_R(OP_OP, rd, 2, rs1, rs2, 0x00)
#define SLTU(rd, rs1, rs2) ENC_R(OP_OP, rd, 3, rs1, rs2, 0x00)
#define ADDI(rd, rs1, imm) ENC_I(OP_OPIMM, rd, 0, rs1, imm)
#define ANDI(rd, rs1, imm) ENC_I(OP_OPIMM, rd, 7, rs1, imm)
#define ORI(rd, rs1, imm) ENC_I(OP_OPIMM, rd, 6, rs1, imm)
#define XORI(rd, rs1, imm) ENC_I(OP_OPIMM, rd, 4, rs1, imm)
#define SLTI(rd, rs1, imm) ENC_I(OP_OPIMM, rd, 2, rs1, imm)
#define SLTIU(rd, rs1, imm) ENC_I(OP_OPIMM, rd, 3, rs1, imm)
#define SLLI(rd, rs1, sh) ENC_I(OP_OPIMM, rd, 1, rs1, sh)
#define SRLI(rd, rs1, sh) ENC_I(OP_OPIMM, rd, 5, rs1, sh)
#define SRAI(rd, rs1, sh) ENC_I(OP_OPIMM, rd, 5, rs1, 0x400 | (sh))
#define LB(rd, rs1, imm) ENC_I(OP_LOAD, rd, 0, rs1, imm)
#define LH(rd, rs1, imm) ENC_I(OP_LOAD, rd, 1, rs1, imm)
#define LW(rd, rs1, imm) ENC_I(OP_LOAD, rd, 2, rs1, imm)
#define LD(rd, rs1, imm) ENC_I(OP_LOAD, rd, 3, rs1, imm)
#define LBU(rd, rs1, imm) ENC_I(OP_LOAD, rd, 4, rs1, imm)
#define LHU(rd, rs1, imm) ENC_I(OP_LOAD, rd, 5, rs1, imm)
#define LWU(rd, rs1, imm) ENC_I(OP_LOAD, rd, 6, rs1, imm)
#define SB(rs2, rs1, imm) ENC_S(OP_STORE, 0, rs1, rs2, imm)
#define SH(rs2, rs1, imm) ENC_S(OP_STORE, 1, rs1, rs2, imm)
#define SW(rs2, rs1, imm) ENC_S(OP_STORE, 2, rs1, rs2, imm)
#define SD(rs2, rs1, imm) ENC_S(OP_STORE, 3, rs1, rs2, imm)
#define BEQ(rs1, rs2, off) ENC_B(OP_BRANCH, 0, rs1, rs2, off)
#define BNE(rs1, rs2, off) ENC_B(OP_BRANCH, 1, rs1, rs2, off)
#define BLT(rs1, rs2, off) ENC_B(OP_BRANCH, 4, rs1, rs2, off)
#define BGE(rs1, rs2, off) ENC_B(OP_BRANCH, 5, rs1, rs2, off)
#define BLTU(rs1, rs2, off) ENC_B(OP_BRANCH, 6, rs1, rs2, off)
#define BGEU(rs1, rs2, off) ENC_B(OP_BRANCH, 7, rs1, rs2, off)
#define JAL(rd, off) ENC_J(OP_JAL, rd, off)
#define JALR(rd, rs1, imm) ENC_I(OP_JALR, rd, 0, rs1, imm)
#define NOP() ADDI(X_ZERO, X_ZERO, 0)
#define ECALL() 0x00000073u
#define EBREAK() 0x00100073u
#define MRET() 0x30200073u
#define SRET() 0x10200073u
#define WFI() 0x10500073u
#define CSRRW(rd, csr, rs1) ENC_I(OP_SYSTEM, rd, 1, rs1, (s32)(csr))
#define CSRRS(rd, csr, rs1) ENC_I(OP_SYSTEM, rd, 2, rs1, (s32)(csr))
#define CSRRC(rd, csr, rs1) ENC_I(OP_SYSTEM, rd, 3, rs1, (s32)(csr))
#define CSRRWI(rd, csr, uimm) ENC_I(OP_SYSTEM, rd, 5, uimm, (s32)(csr))
#define CSRRSI(rd, csr, uimm) ENC_I(OP_SYSTEM, rd, 6, uimm, (s32)(csr))
#define CSRRCI(rd, csr, uimm) ENC_I(OP_SYSTEM, rd, 7, uimm, (s32)(csr))
/* Word (32-bit) forms */
#define ADDW(rd, rs1, rs2) ENC_R(OP_OP32, rd, 0, rs1, rs2, 0x00)
#define SUBW(rd, rs1, rs2) ENC_R(OP_OP32, rd, 0, rs1, rs2, 0x20)
#define ADDIW(rd, rs1, imm) ENC_I(OP_OPIMM32, rd, 0, rs1, imm)
#define SLLIW(rd, rs1, sh) ENC_I(OP_OPIMM32, rd, 1, rs1, sh)
#define SRLIW(rd, rs1, sh) ENC_I(OP_OPIMM32, rd, 5, rs1, sh)
#define SRAIW(rd, rs1, sh) ENC_I(OP_OPIMM32, rd, 5, rs1, 0x400 | (sh))
#define SLLW(rd, rs1, rs2) ENC_R(OP_OP32, rd, 1, rs1, rs2, 0x00)
#define SRLW(rd, rs1, rs2) ENC_R(OP_OP32, rd, 5, rs1, rs2, 0x00)
#define SRAW(rd, rs1, rs2) ENC_R(OP_OP32, rd, 5, rs1, rs2, 0x20)
/* M extension */
#define MUL(rd, rs1, rs2) ENC_R(OP_OP, rd, 0, rs1, rs2, 0x01)
#define MULH(rd, rs1, rs2) ENC_R(OP_OP, rd, 1, rs1, rs2, 0x01)
#define MULHSU(rd, rs1, rs2) ENC_R(OP_OP, rd, 2, rs1, rs2, 0x01)
#define MULHU(rd, rs1, rs2) ENC_R(OP_OP, rd, 3, rs1, rs2, 0x01)
#define DIV(rd, rs1, rs2) ENC_R(OP_OP, rd, 4, rs1, rs2, 0x01)
#define DIVU(rd, rs1, rs2) ENC_R(OP_OP, rd, 5, rs1, rs2, 0x01)
#define REM(rd, rs1, rs2) ENC_R(OP_OP, rd, 6, rs1, rs2, 0x01)
#define REMU(rd, rs1, rs2) ENC_R(OP_OP, rd, 7, rs1, rs2, 0x01)
#define MULW(rd, rs1, rs2) ENC_R(OP_OP32, rd, 0, rs1, rs2, 0x01)
#define DIVW(rd, rs1, rs2) ENC_R(OP_OP32, rd, 4, rs1, rs2, 0x01)
#define DIVUW(rd, rs1, rs2) ENC_R(OP_OP32, rd, 5, rs1, rs2, 0x01)
#define REMW(rd, rs1, rs2) ENC_R(OP_OP32, rd, 6, rs1, rs2, 0x01)
#define REMUW(rd, rs1, rs2) ENC_R(OP_OP32, rd, 7, rs1, rs2, 0x01)
/* A extension */
#define AMO(f5, aqrl, rd, rs2, rs1, w) ENC_R(OP_AMO, rd, (w) ? 3 : 2, rs1, rs2, ((f5) << 2) | (aqrl))
#define LR_W(rd, rs1) AMO(0x02, 0, rd, 0, rs1, 0)
#define SC_W(rd, rs2, rs1) AMO(0x03, 0, rd, rs2, rs1, 0)
#define LR_D(rd, rs1) AMO(0x02, 0, rd, 0, rs1, 1)
#define SC_D(rd, rs2, rs1) AMO(0x03, 0, rd, rs2, rs1, 1)
#define AMOSWAP_W(rd, rs2, rs1) AMO(0x01, 0, rd, rs2, rs1, 0)
#define AMOADD_W(rd, rs2, rs1) AMO(0x00, 0, rd, rs2, rs1, 0)
#define AMOXOR_W(rd, rs2, rs1) AMO(0x04, 0, rd, rs2, rs1, 0)
#define AMOAND_W(rd, rs2, rs1) AMO(0x0C, 0, rd, rs2, rs1, 0)
#define AMOOR_W(rd, rs2, rs1) AMO(0x08, 0, rd, rs2, rs1, 0)
#define AMOMIN_W(rd, rs2, rs1) AMO(0x10, 0, rd, rs2, rs1, 0)
#define AMOMAX_W(rd, rs2, rs1) AMO(0x14, 0, rd, rs2, rs1, 0)
#define AMOMINU_W(rd, rs2, rs1) AMO(0x18, 0, rd, rs2, rs1, 0)
#define AMOMAXU_W(rd, rs2, rs1) AMO(0x1C, 0, rd, rs2, rs1, 0)
#define AMOADD_D(rd, rs2, rs1) AMO(0x00, 0, rd, rs2, rs1, 1)
#define AMOSWAP_D(rd, rs2, rs1) AMO(0x01, 0, rd, rs2, rs1, 1)
/* F/D */
#define FLD(rd, rs1, imm) ENC_I(OP_LOADFP, rd, 3, rs1, imm)
#define FSD(rs2, rs1, imm) ENC_S(OP_STOREFP, 3, rs1, rs2, imm)
#define FLW(rd, rs1, imm) ENC_I(OP_LOADFP, rd, 2, rs1, imm)
#define FSW(rs2, rs1, imm) ENC_S(OP_STOREFP, 2, rs1, rs2, imm)
#define FADD_D(rd, rs1, rs2, rm) ENC_R(OP_OPFP, rd, rm, rs1, rs2, 0x01)
#define FSUB_D(rd, rs1, rs2, rm) ENC_R(OP_OPFP, rd, rm, rs1, rs2, 0x05)
#define FMUL_D(rd, rs1, rs2, rm) ENC_R(OP_OPFP, rd, rm, rs1, rs2, 0x09)
#define FDIV_D(rd, rs1, rs2, rm) ENC_R(OP_OPFP, rd, rm, rs1, rs2, 0x0D)
#define FADD_S(rd, rs1, rs2, rm) ENC_R(OP_OPFP, rd, rm, rs1, rs2, 0x00)
#define FMUL_S(rd, rs1, rs2, rm) ENC_R(OP_OPFP, rd, rm, rs1, rs2, 0x08)
#define FSQRT_D(rd, rs1, rm) ENC_R(OP_OPFP, rd, rm, rs1, 0, 0x2D)
#define FSQRT_S(rd, rs1, rm) ENC_R(OP_OPFP, rd, rm, rs1, 0, 0x2C)
#define FCVT_D_W(rd, rs1, rm) ENC_R(OP_OPFP, rd, rm, rs1, 0, 0x69)
#define FCVT_S_W(rd, rs1, rm) ENC_R(OP_OPFP, rd, rm, rs1, 0, 0x68)
#define FCVT_W_D(rd, rs1, rm) ENC_R(OP_OPFP, rd, rm, rs1, 0, 0x61)
#define FMV_X_D(rd, rs1) ENC_R(OP_OPFP, rd, 0, rs1, 0, 0x71)
#define FMV_D_X(rd, rs1) ENC_R(OP_OPFP, rd, 0, rs1, 0, 0x79)
#define FMV_X_W(rd, rs1) ENC_R(OP_OPFP, rd, 0, rs1, 0, 0x70)
#define FMV_W_X(rd, rs1) ENC_R(OP_OPFP, rd, 0, rs1, 0, 0x78)
#define FCLASS_S(rd, rs1) ENC_R(OP_OPFP, rd, 1, rs1, 0, 0x70)
#define FEQ_S(rd, rs1, rs2) ENC_R(OP_OPFP, rd, 2, rs1, rs2, 0x24)
#define FLT_S(rd, rs1, rs2) ENC_R(OP_OPFP, rd, 1, rs1, rs2, 0x24)
#define FLE_S(rd, rs1, rs2) ENC_R(OP_OPFP, rd, 0, rs1, rs2, 0x24)
#define FEQ_D(rd, rs1, rs2) ENC_R(OP_OPFP, rd, 2, rs1, rs2, 0x25)
#define FLT_D(rd, rs1, rs2) ENC_R(OP_OPFP, rd, 1, rs1, rs2, 0x25)
#define FLE_D(rd, rs1, rs2) ENC_R(OP_OPFP, rd, 0, rs1, rs2, 0x25)
#define FMIN_D(rd, rs1, rs2, rm) ENC_R(OP_OPFP, rd, rm, rs1, rs2, 0x15)
#define FSGNJ_D(rd, rs1, rs2, mode) ENC_R(OP_OPFP, rd, mode, rs1, rs2, 0x11)
#define FMADD_D(rd, rs1, rs2, rs3, rm)                                                             \
    ((((u32)(rs3)&0x1F) << 27) | ENC_R(OP_FMADD, rd, rm, rs1, rs2, 0x01))

#endif /* RVM_HARNESS_H */
