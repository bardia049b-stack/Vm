/*
 * loader.c -- ELF/blob loading plus the RVM device tree.
 *
 * SPDX-License-Identifier: MIT
 */
#include "loader.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/* phandle assignments must be stable: Linux resolves &cpu_intc / &plic by them. */
#define PHANDLE_CPU_INTC 1
#define PHANDLE_PLIC     2

static rvm_err slurp(const char *path, u8 **out, size_t *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        LOG_ERROR("loader: cannot open %s: %s", path, strerror(errno));
        return RVM_ERR_IO;
    }
    struct stat sb;
    if (fstat(fd, &sb) != 0) {
        close(fd);
        return RVM_ERR_IO;
    }
    size_t n = (size_t)sb.st_size;
    u8 *buf = (u8 *)malloc(n ? n : 1);
    if (!buf) {
        close(fd);
        return RVM_ERR_NOMEM;
    }
    size_t done = 0;
    while (done < n) {
        ssize_t r = read(fd, buf + done, n - done);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            free(buf);
            close(fd);
            return RVM_ERR_IO;
        }
        if (r == 0)
            break;
        done += (size_t)r;
    }
    close(fd);
    *out = buf;
    *len = done;
    return RVM_OK;
}

/* Minimal ELF64 header, read field by field to stay endianness-explicit. */
typedef struct {
    u8 e_ident[16];
    u16 e_type, e_machine, e_version;
    u64 e_entry, e_phoff, e_shoff;
    u32 e_flags;
    u16 e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} elf64_hdr;

static u16 rd16(const u8 *p) {
    return (u16)(p[0] | (p[1] << 8));
}
static u32 rd32(const u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static u64 rd64(const u8 *p) {
    return (u64)rd32(p) | ((u64)rd32(p + 4) << 32);
}

rvm_err loader_load_elf(bus *b, const char *path, u64 *entry, loader_stats *st) {
    u8 *img = NULL;
    size_t imglen = 0;
    rvm_err e = slurp(path, &img, &imglen);
    if (e != RVM_OK)
        return e;

    if (st)
        memset(st, 0, sizeof(*st));

    if (imglen < sizeof(elf64_hdr)) {
        LOG_ERROR("loader: %s is too small to be an ELF64 image", path);
        free(img);
        return RVM_ERR_BADARG;
    }
    elf64_hdr h;
    memcpy(h.e_ident, img, 16);
    h.e_type = rd16(img + 16);
    h.e_machine = rd16(img + 18);
    h.e_version = rd32(img + 20);
    h.e_entry = rd64(img + 24);
    h.e_phoff = rd64(img + 32);
    h.e_shoff = rd64(img + 40);
    h.e_flags = rd32(img + 48);
    h.e_ehsize = rd16(img + 52);
    h.e_phentsize = rd16(img + 54);
    h.e_phnum = rd16(img + 56);

    if (h.e_ident[0] != ELF_MAGIC0 || memcmp(h.e_ident + 1, "ELF", 3) != 0) {
        LOG_ERROR("loader: %s is not an ELF file", path);
        free(img);
        return RVM_ERR_BADARG;
    }
    if (h.e_ident[4] != ELFCLASS64 || h.e_ident[5] != ELFDATA2LSB) {
        LOG_ERROR("loader: %s is not ELF64 little-endian", path);
        free(img);
        return RVM_ERR_BADARG;
    }
    if (h.e_machine != EM_RISCV) {
        LOG_WARN("loader: %s e_machine=%u, expected %u (RISC-V)", path, h.e_machine, EM_RISCV);
    }

    for (u32 i = 0; i < h.e_phnum; i++) {
        size_t off = (size_t)h.e_phoff + (size_t)i * h.e_phentsize;
        if (off + 56 > imglen)
            break;
        const u8 *ph = img + off;
        u32 type = rd32(ph + 0);
        u64 p_offset = rd64(ph + 8);
        u64 p_vaddr = rd64(ph + 16);
        u64 p_paddr = rd64(ph + 24);
        u64 p_filesz = rd64(ph + 32);
        u64 p_memsz = rd64(ph + 40);
        if (type != PT_LOAD)
            continue;
        if (p_filesz == 0 && p_memsz == 0)
            continue;

        u64 dest = p_paddr ? p_paddr : p_vaddr;
        if (p_offset + p_filesz > imglen) {
            LOG_ERROR("loader: segment %u of %s extends past EOF", i, path);
            free(img);
            return RVM_ERR_BADARG;
        }
        if (!bus_ram_valid(b, dest, (u32)RVM_MIN(p_memsz, (u64)UINT32_MAX))) {
            LOG_ERROR("loader: segment %u target 0x%llx (+%llu) is outside RAM", i,
                      (unsigned long long)dest, (unsigned long long)p_memsz);
            free(img);
            return RVM_ERR_RANGE;
        }
        u8 *dst = bus_ram_ptr(b, dest);
        if (p_filesz)
            memcpy(dst, img + p_offset, (size_t)p_filesz);
        if (p_memsz > p_filesz)
            memset(dst + p_filesz, 0, (size_t)(p_memsz - p_filesz));

        if (st) {
            st->segments++;
            st->bytes += p_memsz;
            if (st->lowest == 0 || dest < st->lowest)
                st->lowest = dest;
            if (dest + p_memsz > st->highest)
                st->highest = dest + p_memsz;
        }
        LOG_DEBUG("loader: LOAD vaddr=0x%llx paddr=0x%llx filesz=%llu memsz=%llu",
                  (unsigned long long)p_vaddr, (unsigned long long)dest,
                  (unsigned long long)p_filesz, (unsigned long long)p_memsz);
    }

    if (entry)
        *entry = h.e_entry;
    LOG_INFO("loader: %s -> entry 0x%llx, %llu segment(s), %llu KiB", path,
             (unsigned long long)h.e_entry, st ? (unsigned long long)st->segments : 0ULL,
             st ? (unsigned long long)(st->bytes >> 10) : 0ULL);
    free(img);
    return RVM_OK;
}

rvm_err loader_load_blob(bus *b, const char *path, u64 addr, u64 *size) {
    u8 *img = NULL;
    size_t imglen = 0;
    rvm_err e = slurp(path, &img, &imglen);
    if (e != RVM_OK)
        return e;
    if (!bus_ram_valid(b, addr, (u32)imglen)) {
        LOG_ERROR("loader: blob %s (%zu bytes) does not fit at 0x%llx", path, imglen,
                  (unsigned long long)addr);
        free(img);
        return RVM_ERR_RANGE;
    }
    memcpy(bus_ram_ptr(b, addr), img, imglen);
    if (size)
        *size = imglen;
    LOG_INFO("loader: blob %s -> 0x%llx (%zu bytes)", path, (unsigned long long)addr, imglen);
    free(img);
    return RVM_OK;
}

/* ------------------------------------------------- PE/COFF (EFI stub) */

#define PE_MZ           0x5A4Du /* the Image header's first instruction */
#define PE_OPT_MAGIC_64 0x020Bu /* PE32+ */
#define PE_MACHINE_RV64 0x5064u /* IMAGE_FILE_MACHINE_RISCV64 */

loader_kind loader_sniff(const char *path) {
    u8 head[64];
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return LOADER_KIND_UNKNOWN;
    ssize_t n = read(fd, head, sizeof head);
    close(fd);
    if (n < 4)
        return LOADER_KIND_UNKNOWN;
    if (head[0] == ELF_MAGIC0 && memcmp(head + 1, "ELF", 3) == 0)
        return LOADER_KIND_ELF;
    u16 mz = (u16)(head[0] | (head[1] << 8));
    if (mz == PE_MZ && n >= 64 && memcmp(head + 0x30, "RISCV", 5) == 0)
        return LOADER_KIND_PE;
    return LOADER_KIND_BLOB;
}

rvm_err loader_load_pe(bus *b, const char *path, u64 base, u64 *entry, loader_stats *st) {
    u8 *img = NULL;
    size_t imglen = 0;
    rvm_err rc = slurp(path, &img, &imglen);
    if (rc != RVM_OK)
        return rc;
    if (st)
        memset(st, 0, sizeof(*st));

    rvm_err e = RVM_ERR_BADARG;
    if (imglen < 0x40 || (u16)(img[0] | (img[1] << 8)) != PE_MZ)
        goto done;
    u64 lfanew = rd32(img + 0x3C);
    if (lfanew + 24 > imglen || memcmp(img + lfanew, "PE\0\0", 4) != 0)
        goto done;

    u16 machine = rd16(img + lfanew + 4);
    u16 nsec = rd16(img + lfanew + 6);
    u16 optsz = rd16(img + lfanew + 20);
    u64 opt = lfanew + 24;
    if (machine != PE_MACHINE_RV64) {
        LOG_ERROR("loader: %s is PE machine 0x%x, not riscv64", path, machine);
        goto done;
    }
    if (opt + 2 > imglen || rd16(img + opt) != PE_OPT_MAGIC_64) {
        LOG_ERROR("loader: %s is not PE32+", path);
        goto done;
    }
    /* The RISCV magic is what separates a kernel Image from any other EFI
     * application; without it we would happily load firmware. */
    if (memcmp(img + 0x30, "RISCV", 5) != 0) {
        LOG_ERROR("loader: %s has no RISCV Image magic at 0x30", path);
        goto done;
    }
    u64 image_size = rd64(img + 0x10);

    u64 sec = opt + optsz;
    u64 min_va = UINT64_MAX, span_end = 0, raw_end = 0;
    for (u16 i = 0; i < nsec; i++) {
        u64 s = sec + (u64)i * 40;
        if (s + 40 > imglen) {
            LOG_ERROR("loader: %s section table runs past EOF", path);
            goto done;
        }
        u64 va = rd32(img + s + 12);
        u64 vsize = rd32(img + s + 8);
        u64 rawptr = rd32(img + s + 20);
        u64 rawsz = rd32(img + s + 16);
        if (rawptr + rawsz > imglen) {
            LOG_ERROR("loader: %s section %u extends past EOF", path, i);
            goto done;
        }
        if (va < min_va)
            min_va = va;
        if (va + vsize > span_end)
            span_end = va + vsize;
        if (rawptr + rawsz > raw_end)
            raw_end = rawptr + rawsz;
    }
    if (min_va == UINT64_MAX || span_end == 0) {
        LOG_ERROR("loader: %s has no loadable sections", path);
        goto done;
    }
    if (!bus_ram_valid(b, base, (u32)RVM_MIN(span_end, (u64)UINT32_MAX))) {
        LOG_ERROR("loader: %s needs 0x%llx bytes at 0x%llx, outside RAM", path,
                  (unsigned long long)span_end, (unsigned long long)base);
        e = RVM_ERR_RANGE;
        goto done;
    }

    /* Headers first: code0 at offset 0 is the non-EFI kernel entry. */
    memcpy(bus_ram_ptr(b, base), img, (size_t)min_va);
    for (u16 i = 0; i < nsec; i++) {
        u64 s = sec + (u64)i * 40;
        u64 va = rd32(img + s + 12);
        u64 vsize = rd32(img + s + 8);
        u64 rawptr = rd32(img + s + 20);
        u64 rawsz = rd32(img + s + 16);
        u8 *dst = bus_ram_ptr(b, base + va);
        if (rawsz)
            memcpy(dst, img + rawptr, (size_t)rawsz);
        if (vsize > rawsz)
            memset(dst + rawsz, 0, (size_t)(vsize - rawsz));
        if (st) {
            st->segments++;
            st->bytes += rawsz;
        }
    }
    if (st) {
        st->lowest = base;
        st->highest = base + span_end;
    }
    /*
     * Enter at base, i.e. at the DOS header itself.  That is not a paradox:
     * head.S starts with
     *
     *     c.li s4, -13      # decodes to the ASCII "MZ" UEFI requires
     *     j   _start_kernel
     *
     * and in the PE layout everything between that and the first section is
     * the EFI header, so _start_kernel lands a few KB into .text.  Entering at
     * the first section instead starts execution inside relocate_enable_mmu,
     * which writes satp from a trampoline table setup_vm never filled and
     * spins in a page-fault loop at its own 1: label.
     */
    if (entry)
        *entry = base;

    LOG_INFO("loader: %s is a PE32+ EFI-stub Image: %u sections, %llu byte image "
             "at 0x%llx, entry 0x%llx",
             path, nsec, (unsigned long long)(image_size ? image_size : span_end),
             (unsigned long long)base, (unsigned long long)base);
    e = RVM_OK;

done:
    free(img);
    return e;
}

/* ------------------------------------------------------- device tree */

rvm_err dtb_build(const dtb_opts *o, u8 **out, u32 *out_len) {
    if (!o || !out || !out_len)
        return RVM_ERR_BADARG;

    fdt f;
    rvm_err e = fdt_init(&f);
    if (e != RVM_OK)
        return e;

    const char *compat_root[] = {o->model ? o->model : "rvm,virt", "virtio-mmio"};
    const char *compat_clint[] = {"riscv,clint0", "sifive,clint"};
    const char *compat_plic[] = {"sifive,plic-1.0.0", "riscv,plic0"};

    /* ---- root ---- */
    fdt_begin_node(&f, "");
    fdt_prop_u32(&f, "#address-cells", 2);
    fdt_prop_u32(&f, "#size-cells", 2);
    fdt_prop_strlist(&f, "compatible", compat_root, 2);
    fdt_prop_str(&f, "model", o->model ? o->model : "rvm,virt");

    /* ---- chosen ---- */
    fdt_begin_node(&f, "chosen");
    if (o->bootargs && *o->bootargs)
        fdt_prop_str(&f, "bootargs", o->bootargs);
    if (o->stdout_path && *o->stdout_path)
        fdt_prop_str(&f, "stdout-path", o->stdout_path);
    if (o->initrd_start && o->initrd_end > o->initrd_start) {
        fdt_prop_u64(&f, "linux,initrd-start", o->initrd_start);
        fdt_prop_u64(&f, "linux,initrd-end", o->initrd_end);
    }
    fdt_end_node(&f);

    /* ---- cpus ---- */
    fdt_begin_node(&f, "cpus");
    fdt_prop_u32(&f, "#address-cells", 1);
    fdt_prop_u32(&f, "#size-cells", 0);
    fdt_prop_u32(&f, "timebase-frequency", o->timebase_hz ? o->timebase_hz : 10000000u);

    fdt_begin_node(&f, "cpu@0");
    fdt_prop_str(&f, "device_type", "cpu");
    fdt_prop_u32(&f, "reg", 0);
    fdt_prop_str(&f, "status", "okay");
    fdt_prop_str(&f, "compatible", "riscv");
    fdt_prop_str(&f, "riscv,isa", o->isa ? o->isa : "rv64imafdc");
    fdt_prop_str(&f, "mmu-type", o->mmu_type ? o->mmu_type : "riscv,sv57");
    fdt_begin_node(&f, "interrupt-controller");
    fdt_prop_u32(&f, "phandle", PHANDLE_CPU_INTC);
    fdt_prop_u32(&f, "#interrupt-cells", 1);
    fdt_prop_empty(&f, "interrupt-controller");
    fdt_prop_str(&f, "compatible", "riscv,cpu-intc");
    fdt_end_node(&f);
    fdt_end_node(&f); /* cpu@0 */
    fdt_end_node(&f); /* cpus  */

    /* ---- memory ---- */
    char memname[64];
    snprintf(memname, sizeof(memname), "memory@%llx", (unsigned long long)o->ram_base);
    fdt_begin_node(&f, memname);
    fdt_prop_str(&f, "device_type", "memory");
    fdt_prop_reg64(&f, "reg", o->ram_base, o->ram_size);
    fdt_end_node(&f);

    /* ---- soc ---- */
    fdt_begin_node(&f, "soc");
    fdt_prop_u32(&f, "#address-cells", 2);
    fdt_prop_u32(&f, "#size-cells", 2);
    fdt_prop_str(&f, "compatible", "simple-bus");
    fdt_prop_empty(&f, "ranges");

    /* CLINT: interrupts-extended = <&cpu_intc 3>, <&cpu_intc 7> */
    fdt_begin_node(&f, "clint@2000000");
    fdt_prop_strlist(&f, "compatible", compat_clint, 2);
    fdt_prop_reg64(&f, "reg", RVM_CLINT_BASE, RVM_CLINT_SIZE);
    fdt_prop_empty(&f, "interrupt-controller");
    {
        u32 cells[4] = {PHANDLE_CPU_INTC, 3, PHANDLE_CPU_INTC, 7};
        fdt_prop_cells(&f, "interrupts-extended", cells, 4);
    }
    fdt_end_node(&f);

    /* PLIC */
    fdt_begin_node(&f, "plic@c000000");
    fdt_prop_strlist(&f, "compatible", compat_plic, 2);
    fdt_prop_reg64(&f, "reg", RVM_PLIC_BASE, RVM_PLIC_SIZE);
    fdt_prop_u32(&f, "#interrupt-cells", 1);
    fdt_prop_u32(&f, "#address-cells", 0);
    fdt_prop_empty(&f, "interrupt-controller");
    fdt_prop_u32(&f, "phandle", PHANDLE_PLIC);
    fdt_prop_u32(&f, "riscv,ndev", RVM_PLIC_MAX_SRC - 1);
    {
        u32 cells[4] = {PHANDLE_CPU_INTC, 11, PHANDLE_CPU_INTC, 9};
        fdt_prop_cells(&f, "interrupts-extended", cells, 4);
    }
    fdt_end_node(&f);

    /* UART */
    fdt_begin_node(&f, "serial@10000000");
    fdt_prop_str(&f, "compatible", "ns16550a");
    fdt_prop_reg64(&f, "reg", RVM_UART_BASE, RVM_UART_SIZE);
    fdt_prop_u32(&f, "interrupt-parent", PHANDLE_PLIC);
    fdt_prop_u32(&f, "interrupts", RVM_UART_IRQ);
    fdt_prop_u32(&f, "clock-frequency", 1843200u);
    fdt_prop_empty(&f, "no-loopback-test");
    fdt_end_node(&f);

    /* virtio-mmio slots */
    u32 n = o->n_virtio ? o->n_virtio : RVM_VIRTIO_COUNT;
    if (n > DTB_MAX_VIRTIO)
        n = DTB_MAX_VIRTIO;
    for (u32 i = 0; i < n; i++) {
        u64 base = RVM_VIRTIO_BASE + i * RVM_VIRTIO_STRIDE;
        char nm[48];
        snprintf(nm, sizeof(nm), "virtio_mmio@%llx", (unsigned long long)base);
        fdt_begin_node(&f, nm);
        fdt_prop_str(&f, "compatible", "virtio,mmio");
        fdt_prop_reg64(&f, "reg", base, RVM_VIRTIO_STRIDE);
        fdt_prop_u32(&f, "interrupt-parent", PHANDLE_PLIC);
        fdt_prop_u32(&f, "interrupts", RVM_VIRTIO_IRQ(i));
        fdt_end_node(&f);
    }

    fdt_end_node(&f); /* soc  */
    fdt_end_node(&f); /* root */

    if (f.error) {
        fdt_free(&f);
        return RVM_ERR_NOMEM;
    }
    e = fdt_finish(&f, out, out_len);
    fdt_free(&f);
    return e;
}
