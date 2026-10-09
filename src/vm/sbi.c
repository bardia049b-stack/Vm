/* SPDX-License-Identifier: MIT */
#include "sbi.h"

static void ret(cpu *c, s64 error, u64 value)
{
    c->x[10] = (u64)error;
    c->x[11] = value;
}

bool sbi_handle(sbi *s, cpu *c)
{
    u64 eid = c->x[17]; /* a7 */
    u64 fid = c->x[16]; /* a6 */
    u64 a0 = c->x[10], a1 = c->x[11], a2 = c->x[12];
    s->n_calls++;

    /* ------------------------------------------------ legacy v0.1 console */
    if (eid == SBI_LEGACY_CONSOLE_PUTCHAR) {
        u8 ch = (u8)a0;
        if (s->write) s->write(s->write_ud, &ch, 1);
        c->x[10] = 0;
        return true;
    }

    switch (eid) {
    case SBI_EXT_BASE:
        switch (fid) {
        case 0: ret(c, SBI_SUCCESS, 0x200ULL); break; /* SBI spec v2.0 */
        case 1: ret(c, SBI_SUCCESS, 1); break;        /* impl id: RVM */
        case 2: ret(c, SBI_SUCCESS, 1); break;        /* impl version */
        case 3: {                                     /* probe_extension */
            bool have = a0 == SBI_EXT_BASE || a0 == SBI_EXT_TIME || a0 == SBI_EXT_IPI ||
                        a0 == SBI_EXT_RFNC || a0 == SBI_EXT_SRST || a0 == SBI_EXT_DBCN;
            ret(c, SBI_SUCCESS, have ? 1 : 0);
            break;
        }
        case 4: ret(c, SBI_SUCCESS, 0); break; /* mvendorid */
        case 5: ret(c, SBI_SUCCESS, 0x72766dULL); break; /* marchid ("rvm") */
        case 6: ret(c, SBI_SUCCESS, 1); break;           /* mimpid */
        default: ret(c, SBI_ERR_NOT_SUPPORTED, 0); s->n_unsupported++; break;
        }
        return true;

    case SBI_EXT_TIME:
        if (fid == 0) { /* sbi_set_timer: RV64 passes the whole value in a0 */
            if (s->set_timer) s->set_timer(s->set_timer_ud, a0);
            /* Per spec: clear STIP and let the CLINT raise MTIP instead. */
            c->csr[CSR_MIP] &= ~MIP_STIP;
            ret(c, SBI_SUCCESS, 0);
        } else {
            ret(c, SBI_ERR_NOT_SUPPORTED, 0);
            s->n_unsupported++;
        }
        return true;

    case SBI_EXT_IPI:
        /* Single hart: an IPI is just a software interrupt to ourselves. */
        if (fid == 0) {
            c->csr[CSR_MIP] |= MIP_MSIP;
            ret(c, SBI_SUCCESS, 0);
        } else {
            ret(c, SBI_ERR_NOT_SUPPORTED, 0);
            s->n_unsupported++;
        }
        return true;

    case SBI_EXT_RFNC:
        if (fid == 0 || fid == 1) { /* remote_sfence_vma{,_asid} */
            mmu_flush(c->mmu);
            ret(c, SBI_SUCCESS, 0);
        } else {
            ret(c, SBI_ERR_NOT_SUPPORTED, 0);
            s->n_unsupported++;
        }
        return true;

    case SBI_EXT_DBCN:
        switch (fid) {
        case 0: { /* console_write: a0=len, a1=base */
            u64 len = a0, addr = a1;
            if (!bus_ram_valid(s->bus, addr, (u32)RVM_MIN(len, (u64)0x10000))) {
                ret(c, SBI_ERR_INVALID_ADDRESS, 0);
                return true;
            }
            u64 done = 0;
            u8 chunk[256];
            while (done < len) {
                u32 n = (u32)RVM_MIN((u64)sizeof(chunk), len - done);
                if (!bus_read_bytes(s->bus, addr + done, chunk, n)) break;
                if (s->write) s->write(s->write_ud, chunk, n);
                done += n;
            }
            ret(c, SBI_SUCCESS, done);
            break;
        }
        case 2: { /* console_write_byte */
            u8 ch = (u8)a0;
            if (s->write) s->write(s->write_ud, &ch, 1);
            ret(c, SBI_SUCCESS, 1);
            break;
        }
        default:
            ret(c, SBI_ERR_NOT_SUPPORTED, 0);
            s->n_unsupported++;
            break;
        }
        return true;

    case SBI_EXT_SRST:
        if (fid == 0) { /* system_reset(a0=type, a1=reason) */
            LOG_INFO("SBI SRST: type=%llu reason=%llu", (unsigned long long)a0,
                     (unsigned long long)a1);
            if (s->shutdown) return s->shutdown(s->shutdown_ud, (u32)a0, (u32)a1);
            ret(c, SBI_SUCCESS, 0);
        } else {
            ret(c, SBI_ERR_NOT_SUPPORTED, 0);
            s->n_unsupported++;
        }
        return true;

    default:
        LOG_WARN("SBI: unsupported extension 0x%llx fid %llu (a0=0x%llx)",
                 (unsigned long long)eid, (unsigned long long)fid, (unsigned long long)a0);
        RVM_UNUSED(a2);
        ret(c, SBI_ERR_NOT_SUPPORTED, 0);
        s->n_unsupported++;
        return true;
    }
}

void sbi_init(sbi *s, bus *b)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
    s->bus = b;
}
