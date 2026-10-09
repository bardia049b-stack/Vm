/*
 * mmu.h -- Sv39 / Sv48 / Sv57 address translation with a small direct-mapped
 *          TLB.
 *
 * Linux 6.12 on riscv64 defaults to Sv57 when the device tree advertises
 * mmu-type = "riscv,sv57", falls back to Sv48 then Sv39.  All three are
 * implemented here with one shared walk so the mode is decided purely by
 * satp.MODE at run time.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RVM_MMU_H
#define RVM_MMU_H

#include "../rvm.h"

#define MMU_TLB_BITS 6 /* 64 entries, direct mapped -- cheap and cache friendly */
#define MMU_TLB_SIZE (1u << MMU_TLB_BITS)
#define MMU_PAGE_SIZE 4096ULL

struct bus;

typedef struct tlb_entry {
    u64 va_page;  /* VA with the low 12 bits cleared */
    u64 pa_page;  /* translated PA, also page aligned */
    u64 page_len; /* size of the mapped leaf: 4K, 2M, 1G, 512G or 256T */
    u32 perm;     /* PTE_R|PTE_W|PTE_X|PTE_U bits of the leaf */
    u32 mode;     /* satp mode that produced this entry */
    u32 asid;
    u64 tag;      /* monotonically increasing satp signature, invalidates all */
    bool valid;
} tlb_entry;

typedef struct mmu {
    struct bus *bus;
    tlb_entry tlb[MMU_TLB_SIZE];
    u64 tag; /* bumped on every satp write / sfence.vma */

    u64 n_walk, n_tlb_hit, n_tlb_miss, n_fault;
} mmu;

/* Translation outcome.  On failure `cause` holds the exact mcause code that
 * the hart must raise: 12 (instruction), 13 (load) or 15 (store) page fault. */
typedef struct mmu_xlat {
    bool ok;
    u64 pa;
    u32 cause;
} mmu_xlat;

rvm_err mmu_init(mmu *m, struct bus *b);
void mmu_free(mmu *m);

/* Drop every TLB entry.  Called on satp write and on sfence.vma(rs1=x0). */
void mmu_flush(mmu *m);
/* Drop one page (sfence.vma with rs1 != x0). */
void mmu_flush_page(mmu *m, u64 va);

/*
 * Translate `va` for the given effective privilege and access kind.
 *
 *   eff_priv  : PRV_M for bare access (M-mode without MPRV), else PRV_S/PRV_U
 *   satp      : current satp value
 *   mxr,sum   : mstatus.MXR / mstatus.SUM
 *   acc       : ACC_EXEC / ACC_LOAD / ACC_STORE
 */
mmu_xlat mmu_translate(mmu *m, u64 va, u32 eff_priv, u64 satp, bool mxr, bool sum, u32 acc);

#endif /* RVM_MMU_H */
