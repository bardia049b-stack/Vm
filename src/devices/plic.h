/*
 * plic.h -- RISC-V Platform-Level Interrupt Controller (single hart,
 *           M + S contexts).
 * SPDX-License-Identifier: MIT
 */
#ifndef RVM_PLIC_H
#define RVM_PLIC_H

#include "../rvm.h"

#define PLIC_MAX_SRC 64
#define PLIC_NUM_CTX 2 /* ctx 0 = hart0 M-mode, ctx 1 = hart0 S-mode */
#define PLIC_CTX_M 0
#define PLIC_CTX_S 1

typedef struct plic {
    u32 priority[PLIC_MAX_SRC];
    u32 pending[(PLIC_MAX_SRC + 31) / 32];
    u32 enable[PLIC_NUM_CTX][(PLIC_MAX_SRC + 31) / 32];
    u32 threshold[PLIC_NUM_CTX];
    u32 claimed[PLIC_NUM_CTX]; /* source id currently claimed, 0 = none */

    u64 n_raise, n_claim;
} plic;

rvm_err plic_init(plic *p);
bool plic_load(void *dev, u64 off, u32 size, u64 *out);
bool plic_store(void *dev, u64 off, u32 size, u64 val);

/* Edge-triggered raise, as a device would do when it wants attention. */
void plic_raise(plic *p, u32 src);

/* Recompute SEIP/MEIP; returns the MIP bits that should be OR-ed into mip. */
u64 plic_update(plic *p);

#endif /* RVM_PLIC_H */
