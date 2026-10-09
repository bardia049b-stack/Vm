/* SPDX-License-Identifier: MIT */
#include "bus.h"

rvm_err bus_init(bus *b, u64 ram_size)
{
    if (!b || ram_size < RVM_RAM_MIN || ram_size > RVM_RAM_MAX) return RVM_ERR_BADARG;
    memset(b, 0, sizeof(*b));
    /* calloc so the guest starts from zeroed pages, like real firmware does. */
    b->ram = (u8 *)calloc(1, (size_t)ram_size);
    if (!b->ram) return RVM_ERR_NOMEM;
    b->ram_size = ram_size;
    b->ram_base = RVM_RAM_BASE;
    return RVM_OK;
}

void bus_free(bus *b)
{
    if (!b) return;
    free(b->ram);
    memset(b, 0, sizeof(*b));
}

rvm_err bus_attach(bus *b, const char *name, u64 base, u64 size, void *dev,
                   bus_read_fn rd, bus_write_fn wr)
{
    if (!b || !name || size == 0 || (!rd && !wr)) return RVM_ERR_BADARG;
    if (b->ndev >= RVM_BUS_MAX_DEVICES) return RVM_ERR_NOMEM;
    for (u32 i = 0; i < b->ndev; i++) {
        const bus_device *d = &b->devs[i];
        if (base < d->base + d->size && d->base < base + size) {
            LOG_ERROR("bus: region %s [0x%llx,0x%llx) overlaps %s", name,
                      (unsigned long long)base, (unsigned long long)(base + size), d->name);
            return RVM_ERR_RANGE;
        }
    }
    bus_device *d = &b->devs[b->ndev++];
    d->name = name;
    d->base = base;
    d->size = size;
    d->dev = dev;
    d->read = rd;
    d->write = wr;
    LOG_DEBUG("bus: attached %-12s [0x%08llx..0x%08llx)", name, (unsigned long long)base,
              (unsigned long long)(base + size - 1));
    return RVM_OK;
}

const bus_device *bus_find(const bus *b, u64 addr)
{
    for (u32 i = 0; i < b->ndev; i++) {
        const bus_device *d = &b->devs[i];
        if (addr >= d->base && addr < d->base + d->size) return d;
    }
    return NULL;
}

bool bus_load(bus *b, u64 addr, u32 size, u64 *out)
{
    if (size == 0 || size > 8 || (size & (size - 1))) return false;
    if (bus_ram_valid(b, addr, size)) {
        const u8 *p = bus_ram_ptr(b, addr);
        u64 v = 0;
        memcpy(&v, p, size);
        *out = v;
        b->n_ram_load++;
        return true;
    }
    const bus_device *d = bus_find(b, addr);
    if (d && d->read && d->read(d->dev, addr - d->base, size, out)) {
        b->n_mmio_load++;
        return true;
    }
    b->n_fault++;
    LOG_TRACE("bus: load fault at 0x%llx size %u", (unsigned long long)addr, size);
    return false;
}

bool bus_store(bus *b, u64 addr, u32 size, u64 val)
{
    if (size == 0 || size > 8 || (size & (size - 1))) return false;
    if (bus_ram_valid(b, addr, size)) {
        u8 *p = bus_ram_ptr(b, addr);
        memcpy(p, &val, size);
        b->n_ram_store++;
        return true;
    }
    const bus_device *d = bus_find(b, addr);
    if (d && d->write && d->write(d->dev, addr - d->base, size, val)) {
        b->n_mmio_store++;
        return true;
    }
    b->n_fault++;
    LOG_TRACE("bus: store fault at 0x%llx size %u val 0x%llx", (unsigned long long)addr, size,
              (unsigned long long)val);
    return false;
}

bool bus_read_bytes(bus *b, u64 addr, void *dst, size_t n)
{
    if (n == 0) return true;
    if (!bus_ram_valid(b, addr, (u32)n)) {
        /* Crosses into MMIO or outside RAM: fall back to per-byte path. */
        u8 *p = (u8 *)dst;
        for (size_t i = 0; i < n; i++) {
            u64 v = 0;
            if (!bus_load(b, addr + i, 1, &v)) return false;
            p[i] = (u8)v;
        }
        return true;
    }
    memcpy(dst, bus_ram_ptr(b, addr), n);
    return true;
}

bool bus_write_bytes(bus *b, u64 addr, const void *src, size_t n)
{
    if (n == 0) return true;
    if (!bus_ram_valid(b, addr, (u32)n)) {
        const u8 *p = (const u8 *)src;
        for (size_t i = 0; i < n; i++)
            if (!bus_store(b, addr + i, 1, p[i])) return false;
        return true;
    }
    memcpy(bus_ram_ptr(b, addr), src, n);
    return true;
}
