/*
 * test_mmu.c -- Sv39/Sv48 translation, permission faults, superpages and the
 *               TLB.
 *
 * Most checks call mmu_translate() directly with hand-built page tables,
 * which pins down the exact fault code Linux would see.  One test runs real
 * guest instructions through a Sv39 identity map to prove the CPU path is
 * wired up too.
 *
 * SPDX-License-Identifier: MIT
 */
#include "fixtures/harness.h"
#include "test.h"

#define PT_BASE (RVM_RAM_BASE + 0x300000ULL) /* page tables live here */
#define PT_L1 (PT_BASE + 0x1000ULL)
#define PT_L0 (PT_BASE + 0x2000ULL)

#define TEST_VA 0x0000000040000000ULL /* VPN[2]=1, VPN[1]=0, VPN[0]=0 */
#define TEST_PA (RVM_RAM_BASE + 0x200000ULL)

static inline u64 pte(u64 pa, u32 flags) { return ((pa >> 12) << 10) | flags; }

static void put_pte(th *t, u64 tbl, u32 index, u64 p)
{
    th_poke64(t, tbl + 8ULL * index, p);
}

static u64 vpn(u64 va, u32 level) { return (va >> (12 + 9 * level)) & 0x1FF; }

/* Build a Sv39 (or Sv48) mapping va -> pa with the given leaf permissions. */
static u64 build_tables(th *t, u64 va, u64 pa, u32 leaf_flags, u64 mode)
{
    u32 levels = (mode == SATP_MODE_SV39) ? 3 : 4;
    put_pte(t, PT_BASE, (u32)vpn(va, levels - 1), pte(PT_L1, PTE_V));
    put_pte(t, PT_L1, (u32)vpn(va, levels - 2), pte(PT_L0, PTE_V));
    put_pte(t, PT_L0, (u32)vpn(va, 0), pte(pa, leaf_flags | PTE_V));
    return (mode << 60) | (PT_BASE >> 12);
}

void test_mmu_bare(void)
{
    th t;
    CHECK(th_init(&t) == RVM_OK);
    mmu_xlat r = mmu_translate(&t.mmu, 0xDEADBEEFULL, PRV_S, 0, false, false, ACC_LOAD);
    CHECK(r.ok);
    CHECK_U64(r.pa, 0xDEADBEEFULL);
    /* M-mode is always bare, even with a non-zero satp. */
    u64 satp = build_tables(&t, TEST_VA, TEST_PA, PTE_R | PTE_W | PTE_X, SATP_MODE_SV39);
    r = mmu_translate(&t.mmu, TEST_VA, PRV_M, satp, false, false, ACC_LOAD);
    CHECK(r.ok);
    CHECK_U64(r.pa, TEST_VA);
    th_free(&t);
}

void test_mmu_sv39(void)
{
    th t;
    CHECK(th_init(&t) == RVM_OK);
    u64 satp = build_tables(&t, TEST_VA, TEST_PA, PTE_R | PTE_W | PTE_X, SATP_MODE_SV39);

    mmu_xlat r = mmu_translate(&t.mmu, TEST_VA, PRV_S, satp, false, false, ACC_LOAD);
    CHECK(r.ok);
    CHECK_U64(r.pa, TEST_PA);

    /* The page offset must survive translation. */
    r = mmu_translate(&t.mmu, TEST_VA + 0x123, PRV_S, satp, false, false, ACC_LOAD);
    CHECK(r.ok);
    CHECK_U64(r.pa, TEST_PA + 0x123);

    /* An unmapped VA must fault with cause 13. */
    r = mmu_translate(&t.mmu, 0x80000000ULL, PRV_S, satp, false, false, ACC_LOAD);
    CHECK(!r.ok);
    CHECK_U64(r.cause, EXC_LOAD_PAGE_FAULT);

    /* A load sets Accessed but must leave Dirty clear. */
    u64 leaf = th_peek64(&t, PT_L0 + 8 * vpn(TEST_VA, 0));
    CHECK_U64(leaf & PTE_A, PTE_A);
    CHECK_U64(leaf & PTE_D, 0);

    /* Only a store may set Dirty. */
    mmu_flush(&t.mmu);
    r = mmu_translate(&t.mmu, TEST_VA, PRV_S, satp, false, false, ACC_STORE);
    CHECK(r.ok);
    leaf = th_peek64(&t, PT_L0 + 8 * vpn(TEST_VA, 0));
    CHECK_U64(leaf & PTE_A, PTE_A);
    CHECK_U64(leaf & PTE_D, PTE_D);
    th_free(&t);
}

void test_mmu_sv48(void)
{
    th t;
    CHECK(th_init(&t) == RVM_OK);
    /* VA with a non-zero VPN[3]: 0x0000_1000_0000_0000 */
    const u64 va = 0x0000100000000000ULL;
    const u64 pt3 = PT_BASE + 0x3000ULL;
    put_pte(&t, PT_BASE, (u32)vpn(va, 3), pte(pt3, PTE_V));
    put_pte(&t, pt3, (u32)vpn(va, 2), pte(PT_L1, PTE_V));
    put_pte(&t, PT_L1, (u32)vpn(va, 1), pte(PT_L0, PTE_V));
    put_pte(&t, PT_L0, (u32)vpn(va, 0), pte(TEST_PA, PTE_V | PTE_R | PTE_W));
    u64 satp = (SATP_MODE_SV48 << 60) | (PT_BASE >> 12);

    mmu_xlat r = mmu_translate(&t.mmu, va, PRV_S, satp, false, false, ACC_LOAD);
    CHECK(r.ok);
    CHECK_U64(r.pa, TEST_PA);

    /* A non-canonical VA for Sv48 must fault rather than alias. */
    r = mmu_translate(&t.mmu, 0xFFFF000000000000ULL, PRV_S, satp, false, false, ACC_LOAD);
    CHECK(!r.ok);
    CHECK_U64(r.cause, EXC_LOAD_PAGE_FAULT);
    th_free(&t);
}

void test_mmu_permissions(void)
{
    th t;
    CHECK(th_init(&t) == RVM_OK);

    /* Read-only page: a store faults with cause 15. */
    u64 satp = build_tables(&t, TEST_VA, TEST_PA, PTE_R, SATP_MODE_SV39);
    mmu_flush(&t.mmu);
    mmu_xlat r = mmu_translate(&t.mmu, TEST_VA, PRV_S, satp, false, false, ACC_STORE);
    CHECK(!r.ok);
    CHECK_U64(r.cause, EXC_STORE_PAGE_FAULT);
    r = mmu_translate(&t.mmu, TEST_VA, PRV_S, satp, false, false, ACC_LOAD);
    CHECK(r.ok);

    /* Non-executable page: a fetch faults with cause 12. */
    mmu_flush(&t.mmu);
    r = mmu_translate(&t.mmu, TEST_VA, PRV_S, satp, false, false, ACC_EXEC);
    CHECK(!r.ok);
    CHECK_U64(r.cause, EXC_INST_PAGE_FAULT);

    /* W=1,R=0 is a reserved leaf encoding and must fault. */
    mmu_flush(&t.mmu);
    satp = build_tables(&t, TEST_VA, TEST_PA, PTE_W, SATP_MODE_SV39);
    r = mmu_translate(&t.mmu, TEST_VA, PRV_S, satp, false, false, ACC_LOAD);
    CHECK(!r.ok);

    /* A U-mode page seen from S-mode: blocked without SUM, allowed with it. */
    mmu_flush(&t.mmu);
    satp = build_tables(&t, TEST_VA, TEST_PA, PTE_R | PTE_W | PTE_U, SATP_MODE_SV39);
    r = mmu_translate(&t.mmu, TEST_VA, PRV_S, satp, false, false, ACC_LOAD);
    CHECK(!r.ok);
    mmu_flush(&t.mmu);
    r = mmu_translate(&t.mmu, TEST_VA, PRV_S, satp, false, true, ACC_LOAD);
    CHECK(r.ok);
    /* ...but SUM never allows execution of a U page from S-mode. */
    mmu_flush(&t.mmu);
    r = mmu_translate(&t.mmu, TEST_VA, PRV_S, satp, false, true, ACC_EXEC);
    CHECK(!r.ok);

    /* And an S-only page must be inaccessible from U-mode. */
    mmu_flush(&t.mmu);
    satp = build_tables(&t, TEST_VA, TEST_PA, PTE_R | PTE_W | PTE_X, SATP_MODE_SV39);
    r = mmu_translate(&t.mmu, TEST_VA, PRV_U, satp, false, false, ACC_LOAD);
    CHECK(!r.ok);

    /* MXR lets a load succeed from an execute-only page. */
    mmu_flush(&t.mmu);
    satp = build_tables(&t, TEST_VA, TEST_PA, PTE_X, SATP_MODE_SV39);
    r = mmu_translate(&t.mmu, TEST_VA, PRV_S, satp, false, false, ACC_LOAD);
    CHECK(!r.ok);
    mmu_flush(&t.mmu);
    r = mmu_translate(&t.mmu, TEST_VA, PRV_S, satp, true, false, ACC_LOAD);
    CHECK(r.ok);
    th_free(&t);
}

void test_mmu_tlb(void)
{
    th t;
    CHECK(th_init(&t) == RVM_OK);
    u64 satp = build_tables(&t, TEST_VA, TEST_PA, PTE_R | PTE_W | PTE_X, SATP_MODE_SV39);

    mmu_xlat r = mmu_translate(&t.mmu, TEST_VA, PRV_S, satp, false, false, ACC_LOAD);
    CHECK(r.ok);
    CHECK_U64(t.mmu.n_tlb_miss, 1);
    CHECK_U64(t.mmu.n_tlb_hit, 0);

    /* Second translation of the same page must be a TLB hit. */
    r = mmu_translate(&t.mmu, TEST_VA, PRV_S, satp, false, false, ACC_LOAD);
    CHECK(r.ok);
    CHECK_U64(t.mmu.n_tlb_hit, 1);

    /* sfence.vma of a single page invalidates just that entry. */
    mmu_flush_page(&t.mmu, TEST_VA);
    r = mmu_translate(&t.mmu, TEST_VA, PRV_S, satp, false, false, ACC_LOAD);
    CHECK(r.ok);
    CHECK_U64(t.mmu.n_tlb_miss, 2);

    /* A full flush drops everything. */
    mmu_flush(&t.mmu);
    r = mmu_translate(&t.mmu, TEST_VA, PRV_S, satp, false, false, ACC_LOAD);
    CHECK(r.ok);
    CHECK_U64(t.mmu.n_tlb_miss, 3);
    th_free(&t);
}

/*
 * End-to-end: run real instructions in S-mode under a Sv39 identity map built
 * from two 1 GiB superpages.  This proves fetch, load and store all go through
 * the same translation path, including MMIO (the UART lives in superpage 0).
 */
void test_mmu_identity_exec(void)
{
    th t;
    CHECK(th_init(&t) == RVM_OK);
    put_pte(&t, PT_BASE, 0, pte(0x00000000ULL, PTE_V | PTE_R | PTE_W | PTE_X));
    put_pte(&t, PT_BASE, 2, pte(RVM_RAM_BASE, PTE_V | PTE_R | PTE_W | PTE_X));

    t.cpu.priv = PRV_S;
    t.cpu.csr[CSR_SATP] = (SATP_MODE_SV39 << 60) | (PT_BASE >> 12);
    mmu_flush(&t.mmu);

    /* Write "OK\n" to the UART, then load it back through a different VA. */
    th_emit(&t, LUI(X_T0, 0x10000000));
    th_emit(&t, ADDI(X_T1, X_ZERO, 'O'));
    th_emit(&t, SB(X_T1, X_T0, 0));
    th_emit(&t, ADDI(X_T1, X_ZERO, 'K'));
    th_emit(&t, SB(X_T1, X_T0, 0));
    th_emit(&t, ADDI(X_T1, X_ZERO, '\n'));
    th_emit(&t, SB(X_T1, X_T0, 0));
    /* A translated load/store pair on RAM. */
    th_li(&t, X_T2, 0x80200000);
    th_emit(&t, ADDI(X_A0, X_ZERO, 0x123));
    th_emit(&t, SD(X_A0, X_T2, 0));
    th_emit(&t, LD(X_A1, X_T2, 0));
    th_run_all(&t);

    t.out[t.outlen] = 0;
    CHECK_STR((const char *)t.out, "OK\n");
    CHECK_U64(t.cpu.x[X_A1], 0x123);
    CHECK_U64(t.cpu.priv, PRV_S); /* no trap escaped into M-mode */
    CHECK_U64(t.cpu.n_traps, 0);
    th_free(&t);
}
