/*
 * sbi.h -- the built-in M-mode firmware: a minimal SBI v2.0 implementation.
 *
 * Linux runs in S-mode and reaches us with `ecall`; cpu_trap hands the hart to
 * M-mode, vm.c calls sbi_handle(), and we `mret` straight back.  No firmware
 * blob is loaded into guest RAM and mtvec is never actually fetched.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RVM_SBI_H
#define RVM_SBI_H

#include "../cpu/cpu.h"

/* Extension IDs */
#define SBI_EXT_BASE 0x10ULL
#define SBI_EXT_TIME 0x54494D45ULL /* "TIME" */
#define SBI_EXT_IPI  0x735049ULL   /* "sPI"  */
#define SBI_EXT_RFNC 0x52464E43ULL
#define SBI_EXT_SRST 0x53525354ULL
#define SBI_EXT_DBCN 0x4442434EULL

/* Legacy console extension (eid == function id for SBI v0.1) */
#define SBI_LEGACY_CONSOLE_PUTCHAR 0x01ULL

/* SBI error codes */
#define SBI_SUCCESS             0
#define SBI_ERR_FAILED          -1
#define SBI_ERR_NOT_SUPPORTED   -2
#define SBI_ERR_INVALID_PARAM   -3
#define SBI_ERR_DENIED          -4
#define SBI_ERR_INVALID_ADDRESS -5

/* SRST reset types */
#define SBI_SRST_SHUTDOWN 0
#define SBI_SRST_REBOOT   1

typedef struct sbi {
    bus *bus;
    /* Console sink for legacy putchar and DBCN writes. */
    void (*write)(void *ud, const u8 *buf, size_t n);
    void *write_ud;
    /* Asked to power off / reboot; returns false to stop the run loop. */
    bool (*shutdown)(void *ud, u32 type, u32 reason);
    void *shutdown_ud;
    /* Timer programming hook into the CLINT. */
    void (*set_timer)(void *ud, u64 stime);
    void *set_timer_ud;

    u64 n_calls, n_unsupported;
} sbi;

void sbi_init(sbi *s, bus *b);

/*
 * Dispatch the ecall currently pending in `c` (a0..a7 hold the arguments).
 * Writes the SBI return value into a0/a1.  Returns false when the guest asked
 * for a shutdown and the run loop should stop.
 */
bool sbi_handle(sbi *s, cpu *c);

#endif /* RVM_SBI_H */
