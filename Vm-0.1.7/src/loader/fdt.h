/*
 * fdt.h -- a tiny flattened-device-tree writer.
 *
 * Building the DTB in-process instead of shelling out to `dtc` has two wins:
 * the emulator stays dependency-free (essential for the Android build, where
 * there is no dtc), and the memory node can be sized from the actual -m value
 * at run time instead of being baked into a checked-in blob.
 *
 * The on-disk layout produced here is FDT version 17, big-endian, exactly
 * what Linux' libfdt expects.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RVM_FDT_H
#define RVM_FDT_H

#include "../rvm.h"

#define FDT_MAGIC 0xD00DFEEDu

typedef struct fdt {
    u8 *structs;
    u32 structs_len, structs_cap;
    u8 *strings;
    u32 strings_len, strings_cap;
    u32 depth;
    bool error;
} fdt;

rvm_err fdt_init(fdt *f);
void fdt_free(fdt *f);

/* Returns the string-block offset of `name`, interning it. */
u32 fdt_intern(fdt *f, const char *name);

rvm_err fdt_begin_node(fdt *f, const char *name);
rvm_err fdt_end_node(fdt *f);
rvm_err fdt_prop_empty(fdt *f, const char *name);
rvm_err fdt_prop_u32(fdt *f, const char *name, u32 v);
rvm_err fdt_prop_u64(fdt *f, const char *name, u64 v);
rvm_err fdt_prop_cells(fdt *f, const char *name, const u32 *cells, u32 n);
rvm_err fdt_prop_reg64(fdt *f, const char *name, u64 base, u64 size);
rvm_err fdt_prop_str(fdt *f, const char *name, const char *s);
rvm_err fdt_prop_strlist(fdt *f, const char *name, const char *const *strs, u32 n);

/* Serialise into a freshly allocated buffer; caller frees with free(). */
rvm_err fdt_finish(fdt *f, u8 **out, u32 *out_len);

#endif /* RVM_FDT_H */
