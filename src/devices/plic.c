/* SPDX-License-Identifier: MIT */
#include "plic.h"

#include "../cpu/cpu.h" /* MIP_SEIP / MIP_MEIP */

#define PLIC_PRIORITY_OFF 0x000000ULL
#define PLIC_PENDING_OFF 0x001000ULL
#define PLIC_ENABLE_OFF 0x002000ULL
#define PLIC_ENABLE_STRIDE 0x80ULL
#define PLIC_CONTEXT_OFF 0x200000ULL
#define PLIC_CONTEXT_STRIDE 0x1000ULL

rvm_err plic_init(plic *p)
{
    if (!p) return RVM_ERR_BADARG;
    memset(p, 0, sizeof(*p));
    return RVM_OK;
}

static inline bool src_valid(u32 s) { return s > 0 && s < PLIC_MAX_SRC; }

void plic_raise(plic *p, u32 src)
{
    if (!src_valid(src)) return;
    p->pending[src / 32] |= (1u << (src % 32));
    p->n_raise++;
}

/* Highest-priority pending+enabled source for a context (0 when none). */
static u32 best_source(const plic *p, u32 ctx)
{
    u32 best = 0, best_prio = 0;
    for (u32 s = 1; s < PLIC_MAX_SRC; s++) {
        if (!(p->pending[s / 32] & (1u << (s % 32)))) continue;
        if (!(p->enable[ctx][s / 32] & (1u << (s % 32)))) continue;
        u32 pr = p->priority[s];
        if (pr > best_prio && pr > p->threshold[ctx]) {
            best_prio = pr;
            best = s;
        }
    }
    return best;
}

u64 plic_update(plic *p)
{
    u64 bits = 0;
    if (best_source(p, PLIC_CTX_M)) bits |= MIP_MEIP;
    if (best_source(p, PLIC_CTX_S)) bits |= MIP_SEIP;
    return bits;
}

bool plic_load(void *dev, u64 off, u32 size, u64 *out)
{
    plic *p = (plic *)dev;
    if (size != 4) return false;
    *out = 0;

    if (off < PLIC_PENDING_OFF) { /* priority */
        u32 s = (u32)(off / 4);
        if (src_valid(s)) *out = p->priority[s];
        return true;
    }
    if (off < PLIC_ENABLE_OFF) { /* pending */
        u32 w = (u32)((off - PLIC_PENDING_OFF) / 4);
        if (w < RVM_ARRAY_SIZE(p->pending)) *out = p->pending[w];
        return true;
    }
    if (off < PLIC_CONTEXT_OFF) { /* enable */
        u64 rel = off - PLIC_ENABLE_OFF;
        u32 ctx = (u32)(rel / PLIC_ENABLE_STRIDE);
        u32 w = (u32)((rel % PLIC_ENABLE_STRIDE) / 4);
        if (ctx < PLIC_NUM_CTX && w < RVM_ARRAY_SIZE(p->enable[0])) *out = p->enable[ctx][w];
        return true;
    }
    { /* context: threshold or claim */
        u64 rel = off - PLIC_CONTEXT_OFF;
        u32 ctx = (u32)(rel / PLIC_CONTEXT_STRIDE);
        u32 reg = (u32)(rel % PLIC_CONTEXT_STRIDE);
        if (ctx >= PLIC_NUM_CTX) return true;
        if (reg == 0x00) {
            *out = p->threshold[ctx];
        } else if (reg == 0x04) { /* claim */
            u32 s = best_source(p, ctx);
            if (s) {
                p->pending[s / 32] &= ~(1u << (s % 32));
                p->claimed[ctx] = s;
                p->n_claim++;
            }
            *out = s;
            LOG_TRACE("plic: ctx%u claim -> src %u", ctx, s);
        }
        return true;
    }
}

bool plic_store(void *dev, u64 off, u32 size, u64 val)
{
    plic *p = (plic *)dev;
    if (size != 4) return false;
    u32 v = (u32)val;

    if (off < PLIC_PENDING_OFF) {
        u32 s = (u32)(off / 4);
        if (src_valid(s)) p->priority[s] = v & 7;
        return true;
    }
    if (off < PLIC_ENABLE_OFF) return true; /* pending is read-only */
    if (off < PLIC_CONTEXT_OFF) {
        u64 rel = off - PLIC_ENABLE_OFF;
        u32 ctx = (u32)(rel / PLIC_ENABLE_STRIDE);
        u32 w = (u32)((rel % PLIC_ENABLE_STRIDE) / 4);
        if (ctx < PLIC_NUM_CTX && w < RVM_ARRAY_SIZE(p->enable[0])) p->enable[ctx][w] = v;
        return true;
    }
    {
        u64 rel = off - PLIC_CONTEXT_OFF;
        u32 ctx = (u32)(rel / PLIC_CONTEXT_STRIDE);
        u32 reg = (u32)(rel % PLIC_CONTEXT_STRIDE);
        if (ctx >= PLIC_NUM_CTX) return true;
        if (reg == 0x00) {
            p->threshold[ctx] = v & 7;
        } else if (reg == 0x04) { /* complete */
            if (p->claimed[ctx] == v) p->claimed[ctx] = 0;
            LOG_TRACE("plic: ctx%u complete src %llu", ctx, (unsigned long long)v);
        }
        return true;
    }
}
