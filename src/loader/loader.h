/*
 * loader.h -- kernel/DTB/initrd loading and the built-in device tree builder.
 * SPDX-License-Identifier: MIT
 */
#ifndef RVM_LOADER_H
#define RVM_LOADER_H

#include "../bus/bus.h"
#include "fdt.h"

/* ELF64 little-endian identification */
#define ELF_MAGIC0  0x7F
#define ELFCLASS64  2
#define ELFDATA2LSB 1
#define EM_RISCV    243
#define PT_LOAD     1

typedef struct loader_stats {
    u64 segments;
    u64 bytes;
    u64 lowest, highest;
} loader_stats;

/* Load an ELF64 image, honouring p_paddr of every PT_LOAD segment. */
rvm_err loader_load_elf(bus *b, const char *path, u64 *entry, loader_stats *st);

/* Load a raw blob (Linux Image, DTB, initrd) at a guest physical address. */
rvm_err loader_load_blob(bus *b, const char *path, u64 addr, u64 *size);

/* ------------------------------------------------------- device tree */

#define DTB_MAX_VIRTIO 8

typedef struct dtb_opts {
    u64 ram_base;
    u64 ram_size;
    const char *model;    /* "rvm,virt" */
    const char *bootargs; /* kernel command line */
    const char *isa;      /* "rv64imafdc" */
    const char *mmu_type; /* "riscv,sv57" */
    u32 n_virtio;         /* how many virtio-mmio nodes to emit */
    u64 initrd_start;     /* 0 when there is no initrd */
    u64 initrd_end;
    u32 timebase_hz; /* CLINT frequency, 10 MHz by default */
    const char *stdout_path;
} dtb_opts;

/* Build a complete DTB for the RVM machine into a malloc'd buffer. */
rvm_err dtb_build(const dtb_opts *o, u8 **out, u32 *out_len);

#endif /* RVM_LOADER_H */
