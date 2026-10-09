/*
 * bus.h -- system bus: flat RAM plus a small table of MMIO devices.
 *
 * Deliberately simple: a linear scan over at most a dozen regions.  The VM is
 * single threaded, so there is not a single lock anywhere in this file, and
 * RAM accesses are a bounds check plus a memcpy -- no DMA engine, no scatter
 * lists, exactly as PLAN prescribes for a lean emulator.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RVM_BUS_H
#define RVM_BUS_H

#include "../rvm.h"

#define RVM_BUS_MAX_DEVICES 16

struct bus;

/*
 * MMIO callbacks.  `size` is 1, 2, 4 or 8.  Loads return the value through
 * *out; a false return raises a load/store access fault (mcause 5/7).
 */
typedef bool (*bus_read_fn)(void *dev, u64 off, u32 size, u64 *out);
typedef bool (*bus_write_fn)(void *dev, u64 off, u32 size, u64 val);

typedef struct bus_device {
    const char *name;
    u64 base;
    u64 size;
    void *dev;
    bus_read_fn read;
    bus_write_fn write;
} bus_device;

typedef struct bus {
    u8 *ram; /* maps to guest physical RVM_RAM_BASE */
    u64 ram_size;
    u64 ram_base; /* == RVM_RAM_BASE, kept for flexibility */

    bus_device devs[RVM_BUS_MAX_DEVICES];
    u32 ndev;

    /* Statistics, handy for --stats and for the Android HUD. */
    u64 n_ram_load, n_ram_store, n_mmio_load, n_mmio_store, n_fault;
} bus;

rvm_err bus_init(bus *b, u64 ram_size);
void bus_free(bus *b);

rvm_err bus_attach(bus *b, const char *name, u64 base, u64 size, void *dev, bus_read_fn rd,
                   bus_write_fn wr);

/* Guest-physical RAM helpers.  addr must be inside the RAM window. */
static inline bool bus_ram_valid(const bus *b, u64 addr, u32 size) {
    if (addr < b->ram_base)
        return false;
    u64 off = addr - b->ram_base;
    return off + size <= b->ram_size && off + size >= off; /* overflow guard */
}

static inline u8 *bus_ram_ptr(bus *b, u64 addr) {
    return b->ram + (addr - b->ram_base);
}

/* Physical accesses: true on success, false -> access fault. */
bool bus_load(bus *b, u64 addr, u32 size, u64 *out);
bool bus_store(bus *b, u64 addr, u32 size, u64 val);

/* Bulk copy to/from guest RAM (used by virtio and the loader). */
bool bus_read_bytes(bus *b, u64 addr, void *dst, size_t n);
bool bus_write_bytes(bus *b, u64 addr, const void *src, size_t n);

const bus_device *bus_find(const bus *b, u64 addr);

#endif /* RVM_BUS_H */
