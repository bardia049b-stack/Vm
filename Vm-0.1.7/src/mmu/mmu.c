/* SPDX-License-Identifier: MIT */
#include "mmu.h"

#include "../bus/bus.h"
#include "../cpu/cpu.h" /* PTE_* / SATP_MODE_* / ACC_* / EXC_* */

rvm_err mmu_init(mmu *m, struct bus *b) {
    if (!m || !b)
        return RVM_ERR_BADARG;
    memset(m, 0, sizeof(*m));
    m->bus = b;
    m->tag = 1;
    return RVM_OK;
}

void mmu_free(mmu *m) {
    if (!m)
        return;
    memset(m, 0, sizeof(*m));
}

void mmu_flush(mmu *m) {
    m->tag++; /* every cached entry carries an older tag, so all miss */
    for (u32 i = 0; i < MMU_TLB_SIZE; i++)
        m->tlb[i].valid = false;
}

void mmu_flush_page(mmu *m, u64 va) {
    u64 page = va & ~(MMU_PAGE_SIZE - 1);
    tlb_entry *e = &m->tlb[(page >> 12) & (MMU_TLB_SIZE - 1)];
    if (e->valid && e->va_page == page && e->tag == m->tag)
        e->valid = false;
}

static u32 levels_for(u64 mode) {
    switch (mode) {
    case SATP_MODE_SV39:
        return 3;
    case SATP_MODE_SV48:
        return 4;
    case SATP_MODE_SV57:
        return 5;
    default:
        return 0;
    }
}

/* VA must be canonical for the mode, i.e. sign-extended from bit (12+9*lv). */
static bool va_canonical(u64 va, u32 levels) {
    u32 top = 12 + 9 * levels; /* highest meaningful bit index + 1 */
    if (top >= 64)
        return true;
    u64 hi = va >> (top - 1);
    return hi == 0 || hi == ((1ULL << (64 - (top - 1))) - 1);
}

static u32 fault_cause(u32 acc) {
    switch (acc) {
    case ACC_EXEC:
        return EXC_INST_PAGE_FAULT;
    case ACC_LOAD:
        return EXC_LOAD_PAGE_FAULT;
    default:
        return EXC_STORE_PAGE_FAULT;
    }
}

mmu_xlat mmu_translate(mmu *m, u64 va, u32 eff_priv, u64 satp, bool mxr, bool sum, u32 acc) {
    mmu_xlat r = {false, 0, fault_cause(acc)};

    u64 mode = (satp >> 60) & 0xF;
    u32 levels = levels_for(mode);

    /* M-mode always uses the bare scheme, and so does everyone when MODE=0. */
    if (eff_priv == PRV_M || levels == 0) {
        r.ok = true;
        r.pa = va;
        return r;
    }

    if (!va_canonical(va, levels)) {
        m->n_fault++;
        r.cause = fault_cause(acc);
        return r;
    }

    /* ---- TLB lookup (direct mapped on the page number) ---- */
    u64 page = va & ~(MMU_PAGE_SIZE - 1);
    u32 idx = (u32)((page >> 12) & (MMU_TLB_SIZE - 1));
    tlb_entry *e = &m->tlb[idx];
    if (e->valid && e->tag == m->tag && e->mode == (u32)mode && e->va_page == page) {
        m->n_tlb_hit++;
        /* Re-check permissions: SUM/MXR depend on the current mstatus. */
        bool need_u = (eff_priv == PRV_U);
        if (need_u && !(e->perm & PTE_U)) {
            m->n_fault++;
            return r;
        }
        if (!need_u && (e->perm & PTE_U) && !(acc == ACC_LOAD && sum)) {
            /* S-mode may only touch U pages when SUM=1, and never to execute. */
            m->n_fault++;
            return r;
        }
        u32 need = (acc == ACC_EXEC) ? PTE_X : (acc == ACC_STORE) ? PTE_W : PTE_R;
        if (!(e->perm & need)) {
            if (!(acc == ACC_LOAD && mxr && (e->perm & PTE_X))) {
                m->n_fault++;
                return r;
            }
        }
        r.ok = true;
        r.pa = e->pa_page + (va - page);
        return r;
    }
    m->n_tlb_miss++;

    /* ---- Full page-table walk ---- */
    u64 ppn_field = satp & 0x00000FFFFFFFFFFFULL;
    u64 pt_base = ppn_field << 12;

    u64 pte = 0;
    u32 leaf_level = 0;
    bool found = false;

    for (s32 i = (s32)levels - 1; i >= 0; i--) {
        u64 vpn = (va >> (12 + 9 * i)) & 0x1FF;
        u64 pte_addr = pt_base + vpn * 8;
        if (!bus_load(m->bus, pte_addr, 8, &pte)) {
            m->n_fault++;
            return r; /* access fault surfaces as a page fault to the guest */
        }
        m->n_walk++;

        if (!(pte & PTE_V)) {
            m->n_fault++;
            return r;
        }
        bool rw = (pte & (PTE_R | PTE_W)) == PTE_W; /* reserved: W=1,R=0 */
        if (rw) {
            m->n_fault++;
            return r;
        }
        if (pte & (PTE_R | PTE_X)) {
            found = true;
            leaf_level = (u32)i;
            break;
        }
        if (i == 0) { /* pointer at the last level -> not a leaf */
            m->n_fault++;
            return r;
        }
        pt_base = ((pte & PTE_PPN) >> 10) << 12;
    }

    if (!found) {
        m->n_fault++;
        return r;
    }

    /* Misaligned superpage: PPN[leaf_level-1:0] must be zero. */
    u64 pte_ppn = (pte & PTE_PPN) >> 10;
    u64 low_mask = leaf_level ? ((1ULL << (9 * leaf_level)) - 1) : 0;
    if (leaf_level && (pte_ppn & low_mask)) {
        m->n_fault++;
        return r;
    }

    /* ---- permission checks ---- */
    bool u_page = (pte & PTE_U) != 0;
    if (eff_priv == PRV_U && !u_page) {
        m->n_fault++;
        return r;
    }
    if (eff_priv == PRV_S && u_page) {
        if (acc == ACC_EXEC || !sum) {
            m->n_fault++;
            return r;
        }
    }
    if (acc == ACC_EXEC) {
        /* Only X matters here: W=1,X=1 is a legal writable+executable page,
         * which is what a kernel maps before it can apply W^X. */
        if (!(pte & PTE_X)) {
            m->n_fault++;
            return r;
        }
    } else if (acc == ACC_STORE) {
        if (!(pte & PTE_W)) {
            m->n_fault++;
            return r;
        }
    } else { /* ACC_LOAD */
        bool readable = (pte & PTE_R) != 0;
        if (!readable && !(mxr && (pte & PTE_X))) {
            m->n_fault++;
            return r;
        }
    }

    /* A/D are managed by hardware here, which the spec explicitly allows. */
    if (!(pte & PTE_A) || (acc == ACC_STORE && !(pte & PTE_D))) {
        u64 new_pte = pte | PTE_A | ((acc == ACC_STORE) ? PTE_D : 0);
        u64 vpn_acc = 0;
        u64 pte_addr = 0;
        /* Recompute the leaf PTE address for the A/D write-back. */
        u64 base = ppn_field << 12;
        for (s32 i = (s32)levels - 1; i > (s32)leaf_level; i--) {
            vpn_acc = (va >> (12 + 9 * i)) & 0x1FF;
            u64 tmp = 0;
            if (!bus_load(m->bus, base + vpn_acc * 8, 8, &tmp))
                break;
            base = ((tmp & PTE_PPN) >> 10) << 12;
        }
        vpn_acc = (va >> (12 + 9 * leaf_level)) & 0x1FF;
        pte_addr = base + vpn_acc * 8;
        bus_store(m->bus, pte_addr, 8, new_pte);
        pte = new_pte;
    }

    /* ---- build the physical address, honouring superpage offsets ---- */
    u64 pa;
    u64 off_in_page = va & (MMU_PAGE_SIZE - 1);
    if (leaf_level == 0) {
        pa = (pte_ppn << 12) | off_in_page;
    } else {
        u64 mega = (1ULL << (12 + 9 * leaf_level));
        u64 hi_ppn = pte_ppn >> (9 * leaf_level);
        pa = (hi_ppn << (12 + 9 * leaf_level)) | (va & (mega - 1));
    }

    /* ---- install into the TLB ---- */
    e->valid = true;
    e->tag = m->tag;
    e->mode = (u32)mode;
    e->asid = (u32)((satp >> 44) & 0xFFFF);
    e->va_page = page;
    e->page_len = (leaf_level == 0) ? MMU_PAGE_SIZE : (1ULL << (12 + 9 * leaf_level));
    e->pa_page = pa & ~(e->page_len - 1);
    /* TLB entries are page-granular, so re-derive PA on hit at 4K granularity. */
    e->pa_page = (pa - off_in_page);
    e->perm = (u32)(pte & (PTE_R | PTE_W | PTE_X | PTE_U));

    r.ok = true;
    r.pa = pa;
    return r;
}
