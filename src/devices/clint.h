/*
 * clint.h -- Core-Local Interruptor: msip, mtimecmp and the 10 MHz mtime
 *            counter.
 * SPDX-License-Identifier: MIT
 */
#ifndef RVM_CLINT_H
#define RVM_CLINT_H

#include "../rvm.h"

#define CLINT_TIMEBASE_HZ 10000000ULL /* 10 MHz, same as QEMU virt */
#define CLINT_NUM_HARTS 1

#define CLINT_MSIP_OFF 0x0000ULL
#define CLINT_MTIMECMP_OFF 0x4000ULL
#define CLINT_MTIME_OFF 0xBFF8ULL

typedef struct clint {
    u32 msip[CLINT_NUM_HARTS];
    u64 mtimecmp[CLINT_NUM_HARTS];
    u64 mtime;
    u64 base_ns; /* host ns at which mtime == 0 */
    bool free_running;
} clint;

rvm_err clint_init(clint *c);
bool clint_load(void *dev, u64 off, u32 size, u64 *out);
bool clint_store(void *dev, u64 off, u32 size, u64 val);

/* Advance mtime from the host clock and return the MIP bits it drives. */
u64 clint_tick(clint *c);
u64 clint_mtime(const clint *c);

#endif /* RVM_CLINT_H */
