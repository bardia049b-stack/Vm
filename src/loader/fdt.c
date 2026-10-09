/* SPDX-License-Identifier: MIT */
#include "fdt.h"

#define FDT_BEGIN_NODE 1
#define FDT_END_NODE 2
#define FDT_PROP 3
#define FDT_NOP 4
#define FDT_END 9

static bool grow(u8 **buf, u32 *len, u32 *cap, u32 need)
{
    if (*len + need <= *cap) return true;
    u32 nc = *cap ? *cap : 1024;
    while (nc < *len + need) nc *= 2;
    u8 *nb = (u8 *)realloc(*buf, nc);
    if (!nb) return false;
    *buf = nb;
    *cap = nc;
    return true;
}

static bool put(u8 **buf, u32 *len, u32 *cap, const void *data, u32 n)
{
    if (!grow(buf, len, cap, n)) return false;
    memcpy(*buf + *len, data, n);
    *len += n;
    return true;
}

static bool put32(u8 **buf, u32 *len, u32 *cap, u32 v)
{
    u32 be = __builtin_bswap32(v);
    return put(buf, len, cap, &be, 4);
}
rvm_err fdt_init(fdt *f)
{
    if (!f) return RVM_ERR_BADARG;
    memset(f, 0, sizeof(*f));
    return RVM_OK;
}

void fdt_free(fdt *f)
{
    if (!f) return;
    free(f->structs);
    free(f->strings);
    memset(f, 0, sizeof(*f));
}

u32 fdt_intern(fdt *f, const char *name)
{
    /* Linear scan: the string block has a few dozen entries at most. */
    u32 off = 0;
    while (off < f->strings_len) {
        if (strcmp((const char *)f->strings + off, name) == 0) return off;
        off += (u32)strlen((const char *)f->strings + off) + 1;
    }
    u32 len = (u32)strlen(name) + 1;
    if (!put(&f->strings, &f->strings_len, &f->strings_cap, name, len)) f->error = true;
    return off;
}

rvm_err fdt_begin_node(fdt *f, const char *name)
{
    if (!put32(&f->structs, &f->structs_len, &f->structs_cap, FDT_BEGIN_NODE)) return RVM_ERR_NOMEM;
    u32 len = (u32)strlen(name) + 1;
    if (!put(&f->structs, &f->structs_len, &f->structs_cap, name, len)) return RVM_ERR_NOMEM;
    /* Node names are padded to a 4-byte boundary. */
    while ((f->structs_len % 4) != 0) {
        u8 z = 0;
        put(&f->structs, &f->structs_len, &f->structs_cap, &z, 1);
    }
    f->depth++;
    return RVM_OK;
}

rvm_err fdt_end_node(fdt *f)
{
    if (f->depth == 0) return RVM_ERR_BADARG;
    f->depth--;
    return put32(&f->structs, &f->structs_len, &f->structs_cap, FDT_END_NODE) ? RVM_OK
                                                                             : RVM_ERR_NOMEM;
}

static rvm_err prop_raw(fdt *f, const char *name, const void *data, u32 len)
{
    u32 nameoff = fdt_intern(f, name);
    if (f->error) return RVM_ERR_NOMEM;
    if (!put32(&f->structs, &f->structs_len, &f->structs_cap, FDT_PROP)) return RVM_ERR_NOMEM;
    if (!put32(&f->structs, &f->structs_len, &f->structs_cap, len)) return RVM_ERR_NOMEM;
    if (!put32(&f->structs, &f->structs_len, &f->structs_cap, nameoff)) return RVM_ERR_NOMEM;
    if (len && !put(&f->structs, &f->structs_len, &f->structs_cap, data, len)) return RVM_ERR_NOMEM;
    while ((f->structs_len % 4) != 0) {
        u8 z = 0;
        if (!put(&f->structs, &f->structs_len, &f->structs_cap, &z, 1)) return RVM_ERR_NOMEM;
    }
    return RVM_OK;
}

rvm_err fdt_prop_empty(fdt *f, const char *name) { return prop_raw(f, name, NULL, 0); }

rvm_err fdt_prop_u32(fdt *f, const char *name, u32 v)
{
    u32 be = __builtin_bswap32(v);
    return prop_raw(f, name, &be, 4);
}

rvm_err fdt_prop_u64(fdt *f, const char *name, u64 v)
{
    u64 be = __builtin_bswap64(v);
    return prop_raw(f, name, &be, 8);
}

rvm_err fdt_prop_cells(fdt *f, const char *name, const u32 *cells, u32 n)
{
    u32 tmp[16];
    if (n > RVM_ARRAY_SIZE(tmp)) return RVM_ERR_BADARG;
    for (u32 i = 0; i < n; i++) tmp[i] = __builtin_bswap32(cells[i]);
    return prop_raw(f, name, tmp, n * 4);
}

rvm_err fdt_prop_reg64(fdt *f, const char *name, u64 base, u64 size)
{
    u8 buf[16];
    u64 b = __builtin_bswap64(base), s = __builtin_bswap64(size);
    memcpy(buf, &b, 8);
    memcpy(buf + 8, &s, 8);
    return prop_raw(f, name, buf, 16);
}

rvm_err fdt_prop_str(fdt *f, const char *name, const char *s)
{
    return prop_raw(f, name, s, (u32)strlen(s) + 1);
}

rvm_err fdt_prop_strlist(fdt *f, const char *name, const char *const *strs, u32 n)
{
    u32 total = 0;
    for (u32 i = 0; i < n; i++) total += (u32)strlen(strs[i]) + 1;
    u8 *buf = (u8 *)malloc(total ? total : 1);
    if (!buf) return RVM_ERR_NOMEM;
    u32 off = 0;
    for (u32 i = 0; i < n; i++) {
        u32 l = (u32)strlen(strs[i]) + 1;
        memcpy(buf + off, strs[i], l);
        off += l;
    }
    rvm_err e = prop_raw(f, name, buf, total);
    free(buf);
    return e;
}

rvm_err fdt_finish(fdt *f, u8 **out, u32 *out_len)
{
    if (!f || !out || !out_len) return RVM_ERR_BADARG;
    if (f->depth != 0) return RVM_ERR_BADARG;
    if (f->error) return RVM_ERR_NOMEM;

    /* Trailing FDT_END token */
    if (!put32(&f->structs, &f->structs_len, &f->structs_cap, FDT_END)) return RVM_ERR_NOMEM;

    const u32 HDR = 40;
    u32 off_struct = HDR;
    u32 off_strings = off_struct + f->structs_len;
    /* The reservation map lives after the strings block and is just a
     * terminating (0,0) pair for us: 16 bytes. */
    u32 off_rsvmap = off_strings + f->strings_len;
    u32 total = off_rsvmap + 16;

    u8 *blob = (u8 *)calloc(1, total);
    if (!blob) return RVM_ERR_NOMEM;

    u32 p = 0;
    u32 hdr[10];
    hdr[0] = FDT_MAGIC;
    hdr[1] = total;
    hdr[2] = off_struct;
    hdr[3] = off_strings;
    hdr[4] = off_rsvmap;
    hdr[5] = 17; /* version */
    hdr[6] = 16; /* last compatible version */
    hdr[7] = 0;  /* boot cpuid */
    hdr[8] = f->strings_len;
    hdr[9] = f->structs_len;
    for (u32 i = 0; i < 10; i++) {
        u32 be = __builtin_bswap32(hdr[i]);
        memcpy(blob + p, &be, 4);
        p += 4;
    }
    memcpy(blob + off_struct, f->structs, f->structs_len);
    memcpy(blob + off_strings, f->strings, f->strings_len);
    /* reservation map terminator: two zero u64s, already zero from calloc */

    *out = blob;
    *out_len = total;
    return RVM_OK;
}
