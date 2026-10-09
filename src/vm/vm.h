/*
 * vm.h -- the machine: RAM, hart, MMU, CLINT, PLIC, UART, virtio and the
 *         built-in SBI firmware, wired together with a single run loop.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RVM_VM_H
#define RVM_VM_H

#include "../cpu/cpu.h"
#include "../devices/clint.h"
#include "../devices/plic.h"
#include "../devices/uart.h"
#include "../devices/virtio.h"
#include "../devices/virtio_blk.h"
#include "../loader/loader.h"
#include "../mmu/mmu.h"
#include "sbi.h"

/* Host-side console hooks, so the same VM drives stdout, a log file, or an
 * Android Canvas terminal without any platform code inside the core. */
typedef void (*vm_write_fn)(void *ud, const u8 *buf, size_t n);
typedef int (*vm_poll_fn)(void *ud, u8 *buf, size_t max);

typedef struct vm_opts {
    u64 ram_size;

    const char *kernel_path;
    const char *dtb_path;     /* NULL -> build the DTB in-process */
    const char *initrd_path;  /* optional */
    const char *disk_path;    /* optional: enables /dev/vda */
    bool create_disk;
    u64 disk_size;

    const char *bootargs;
    const char *isa;          /* default "rv64imafdc" */
    const char *mmu_type;     /* default "riscv,sv57" */

    u64 entry_override;       /* 0 -> use the ELF entry point */
    u64 dtb_addr;             /* 0 -> RVM_DTB_DEFAULT_ADDR */
    u64 initrd_addr;          /* 0 -> RVM_INITRD_DEFAULT_ADDR */
    bool raw_kernel;          /* true -> kernel_path is a raw Image, not ELF */

    u64 max_insns;            /* 0 -> run forever */
    bool trace;
    rvm_loglevel log_level;

    vm_write_fn write;
    void *write_ud;
    vm_poll_fn poll;
    void *poll_ud;
} vm_opts;

void vm_opts_default(vm_opts *o);

typedef struct vm {
    vm_opts opts;

    bus bus;
    mmu mmu;
    cpu cpu;
    clint clint;
    plic plic;
    uart uart;
    sbi sbi;

    virtio_blk blk;
    virtio vio[RVM_VIRTIO_COUNT];
    bool blk_present;

    u8 *dtb;
    u32 dtb_len;
    u64 dtb_addr;
    u64 entry;

    bool running;
    u32 exit_code;
    u64 insns;
    u64 start_ns;

    /* Line-buffered console sink state */
    u8 outbuf[1024];
    u32 outlen;
} vm;

rvm_err vm_new(vm *v, const vm_opts *o);
rvm_err vm_load(vm *v);
rvm_err vm_run(vm *v);
void vm_stop(vm *v, u32 code);
void vm_free(vm *v);

/* Push host keystrokes into the guest's serial FIFO. */
void vm_console_in(vm *v, const u8 *buf, size_t n);
void vm_print_stats(const vm *v);

#endif /* RVM_VM_H */
