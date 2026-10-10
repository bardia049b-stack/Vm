/*
 * clint.h -- Core-Local Interruptor: msip, mtimecmp and the 10 MHz mtime
 *            counter.
 * SPDX-License-Identifier: MIT
 */
#ifndef RVM_CLINT_H
#define RVM_CLINT_H

#include "../rvm.h"

#define CLINT_TIMEBASE_HZ 10000000ULL /* 10 MHz, same as QEMU virt */
#define CLINT_NUM_HARTS   1

#define CLINT_MSIP_OFF     0x0000ULL
#define CLINT_MTIMECMP_OFF 0x4000ULL
#define CLINT_MTIME_OFF    0xBFF8ULL

typedef struct clint {
    u32 msip[CLINT_NUM_HARTS];
    u64 mtimecmp[CLINT_NUM_HARTS];
    u64 mtime;
    u64 base_ns; /* host ns at which mtime == 0 */
    bool free_running;
    /* Virtual time.  A guest on a phone executes a few tens of millions of
     * instructions a second while the host clock keeps ticking at full speed,
     * so every timer read by the guest has real minutes behind it: its watchdog
     * then reports "BUG: soft lockup - CPU#0 stuck for 67s!" for a process that
     * was merely slow.  Deriving mtime from retired instructions instead makes
     * the guest's clock measure the work it has actually done. */
    bool virtual_time;
    u64 insns0; /* instruction count mtime0 belongs to */
    u64 mtime0; /* mtime when virtual time started */
} clint;

rvm_err clint_init(clint *c);
bool clint_load(void *dev, u64 off, u32 size, u64 *out);
bool clint_store(void *dev, u64 off, u32 size, u64 val);

/* Advance mtime from the host clock, or from retired instructions when
 * virtual_time is set (see clint_set_virtual_time), and return the MIP bits. */
u64 clint_tick(clint *c);

/* Switch the time source.  base is the instruction count mtime==0 sits at, so
 * turning this on part way through does not make the clock jump. */
void clint_set_virtual_time(clint *c, bool on, u64 base);

/* Fold instructions retired since the last call into mtime.  No-op unless
 * virtual time is on, so the run loop can call it every refresh. */
void clint_advance(clint *c, u64 insns_now);

/* How fast virtual time runs, in assumed retired instructions per second.  It
 * is a rate, not a measurement: any value in the right decade keeps the guest's
 * timers honest about its own progress. */
#define CLINT_VIRTUAL_MIPS 50000000ULL

u64 clint_mtime(const clint *c);

#endif /* RVM_CLINT_H */
