/*
 * virtio_blk.c -- the disk that Debian lives on.
 *
 * Requests are processed synchronously on notify: one descriptor chain is
 * popped, the header parsed, pread()/pwrite() issued against a host file, and
 * the chain returned through the used ring.  Synchronous is deliberate -- the
 * guest sees realistic latency, the code has no threads and therefore no
 * locks, and pread/pwrite already give us the offset safety we need.
 *
 * SPDX-License-Identifier: MIT
 */
#include "virtio_blk.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

typedef struct virtio_blk_req_hdr {
    u32 type;
    u32 ioprio;
    u64 sector;
} virtio_blk_req_hdr;

/* ------------------------------------------------------------- backend */

static void blk_read_config(virtio *v, u32 off, u32 len, u8 *out) {
    virtio_blk *b = (virtio_blk *)v->be;
    u8 cfg[64];
    memset(cfg, 0, sizeof(cfg));
    memcpy(cfg + 0, &b->capacity, 8); /* offset 0: capacity (sectors) */
    u32 bs = b->blk_size;
    memcpy(cfg + 20, &bs, 4); /* offset 20: blk_size */
    memset(cfg + 24, 0, 4);   /* topology: all zero is fine */
    if (off < sizeof(cfg)) {
        u32 n = RVM_MIN(len, (u32)(sizeof(cfg) - off));
        memcpy(out, cfg + off, n);
    }
}

static void blk_write_config(virtio *v, u32 off, u32 len, const u8 *in) {
    RVM_UNUSED(v);
    RVM_UNUSED(off);
    RVM_UNUSED(len);
    RVM_UNUSED(in);
    /* Nothing in the block config is driver-writable for us. */
}

static void blk_reset(virtio *v) {
    virtio_blk *b = (virtio_blk *)v->be;
    LOG_DEBUG("virtio-blk: reset (reads=%llu writes=%llu)", (unsigned long long)b->n_read,
              (unsigned long long)b->n_write);
}

static bool do_io(virtio_blk *b, u32 type, u64 sector, u8 *buf, u32 len, bool to_guest) {
    off_t off = (off_t)(sector * VIRTIO_BLK_SECTOR_SIZE);
    u32 done = 0;
    while (done < len) {
        ssize_t n;
        if (to_guest)
            n = pread(b->fd, buf + done, len - done, off + done);
        else
            n = pwrite(b->fd, buf + done, len - done, off + done);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            LOG_ERROR("virtio-blk: %s at sector %llu failed: %s", to_guest ? "read" : "write",
                      (unsigned long long)sector, strerror(errno));
            return false;
        }
        if (n == 0)
            break; /* EOF: leave the remainder zero filled */
        done += (u32)n;
    }
    return true;
}

static void blk_notify(virtio *v, u32 qidx) {
    virtio_blk *b = (virtio_blk *)v->be;
    vq_chain ch;

    while (vq_pop(v, qidx, &ch)) {
        /* Locate the header (first readable), the status byte (last writable)
         * and everything in between as data. */
        s32 hdr_i = -1, status_i = -1;
        for (u32 i = 0; i < ch.n; i++) {
            if (!ch.iov[i].write && hdr_i < 0 && ch.iov[i].len >= sizeof(virtio_blk_req_hdr))
                hdr_i = (s32)i;
            if (ch.iov[i].write && ch.iov[i].len >= 1)
                status_i = (s32)i;
        }
        if (hdr_i < 0 || status_i < 0) {
            u8 st = VIRTIO_BLK_S_IOERR;
            vq_write(v, &ch.iov[ch.n - 1], &st, 1);
            vq_done(v, qidx, &ch, 1);
            b->n_err++;
            continue;
        }

        virtio_blk_req_hdr hdr;
        if (!vq_read(v, &ch.iov[hdr_i], &hdr, sizeof(hdr))) {
            u8 st = VIRTIO_BLK_S_IOERR;
            vq_write(v, &ch.iov[status_i], &st, 1);
            vq_done(v, qidx, &ch, 1);
            b->n_err++;
            continue;
        }

        u8 status = VIRTIO_BLK_S_OK;
        u32 written = 1; /* the status byte is always written back */

        switch (hdr.type) {
        case VIRTIO_BLK_T_IN: {
            b->n_read++;
            for (s32 i = hdr_i + 1; i < status_i; i++) {
                if (!ch.iov[i].write)
                    continue;
                u32 len = ch.iov[i].len;
                u8 *tmp = (u8 *)malloc(len);
                if (!tmp) {
                    status = VIRTIO_BLK_S_IOERR;
                    break;
                }
                bool ok = do_io(b, hdr.type, hdr.sector, tmp, len, true);
                if (ok && vq_write(v, &ch.iov[i], tmp, len)) {
                    written += len;
                    hdr.sector += len / VIRTIO_BLK_SECTOR_SIZE;
                } else {
                    status = VIRTIO_BLK_S_IOERR;
                }
                free(tmp);
                if (status != VIRTIO_BLK_S_OK)
                    break;
            }
            break;
        }
        case VIRTIO_BLK_T_OUT: {
            if (b->readonly) {
                status = VIRTIO_BLK_S_IOERR;
                break;
            }
            b->n_write++;
            for (s32 i = hdr_i + 1; i < status_i; i++) {
                if (ch.iov[i].write)
                    continue;
                u32 len = ch.iov[i].len;
                u8 *tmp = (u8 *)malloc(len);
                if (!tmp) {
                    status = VIRTIO_BLK_S_IOERR;
                    break;
                }
                if (vq_read(v, &ch.iov[i], tmp, len) &&
                    do_io(b, hdr.type, hdr.sector, tmp, len, false)) {
                    hdr.sector += len / VIRTIO_BLK_SECTOR_SIZE;
                } else {
                    status = VIRTIO_BLK_S_IOERR;
                }
                free(tmp);
                if (status != VIRTIO_BLK_S_OK)
                    break;
            }
            break;
        }
        case VIRTIO_BLK_T_FLUSH:
            b->n_flush++;
            if (fsync(b->fd) != 0 && errno != EINVAL && errno != EROFS)
                status = VIRTIO_BLK_S_IOERR;
            break;
        case VIRTIO_BLK_T_GET_ID: {
            for (s32 i = hdr_i + 1; i < status_i; i++) {
                if (!ch.iov[i].write)
                    continue;
                u32 len = RVM_MIN(ch.iov[i].len, (u32)strlen(b->id));
                vq_write(v, &ch.iov[i], b->id, len);
                written += len;
                break;
            }
            break;
        }
        default:
            LOG_WARN("virtio-blk: unsupported request type %u", hdr.type);
            status = VIRTIO_BLK_S_UNSUPP;
            break;
        }

        vq_write(v, &ch.iov[status_i], &status, 1);
        vq_done(v, qidx, &ch, written);
        if (status != VIRTIO_BLK_S_OK)
            b->n_err++;
    }
}

/* --------------------------------------------------------------- setup */

rvm_err virtio_blk_open(virtio_blk *b, const char *path, u64 bytes, bool create) {
    if (!b || !path)
        return RVM_ERR_BADARG;
    memset(b, 0, sizeof(*b));

    int flags = O_RDWR;
    if (create)
        flags |= O_CREAT;
    b->fd = open(path, flags, 0644);
    if (b->fd < 0) {
        if (create) {
            b->fd = open(path, O_RDWR | O_CREAT, 0644);
            if (b->fd < 0) {
                LOG_ERROR("virtio-blk: cannot open %s: %s", path, strerror(errno));
                return RVM_ERR_IO;
            }
        } else {
            LOG_ERROR("virtio-blk: cannot open %s: %s", path, strerror(errno));
            return RVM_ERR_IO;
        }
    }

    off_t sz = lseek(b->fd, 0, SEEK_END);
    if (sz < 0)
        sz = 0;
    if (create && bytes && (u64)sz < bytes) {
        if (ftruncate(b->fd, (off_t)bytes) != 0) {
            LOG_ERROR("virtio-blk: cannot size %s to %llu bytes: %s", path,
                      (unsigned long long)bytes, strerror(errno));
            close(b->fd);
            b->fd = -1;
            return RVM_ERR_IO;
        }
        sz = (off_t)bytes;
    }
    lseek(b->fd, 0, SEEK_SET);

    b->capacity = (u64)sz / VIRTIO_BLK_SECTOR_SIZE;
    b->blk_size = VIRTIO_BLK_SECTOR_SIZE;
    snprintf(b->id, sizeof(b->id), "rvm-vdisk");

    b->be.device_id = VIRTIO_ID_BLOCK;
    b->be.nqueues = 1;
    b->be.config_len = 60;
    b->be.features = (1ULL << VIRTIO_BLK_F_BLK_SIZE) | (1ULL << VIRTIO_BLK_F_FLUSH) |
                     (1ULL << VIRTIO_F_VERSION_1);
    b->be.priv = b;
    b->be.notify = blk_notify;
    b->be.reset = blk_reset;
    b->be.read_config = blk_read_config;
    b->be.write_config = blk_write_config;

    LOG_INFO("virtio-blk: %s -> %llu sectors (%llu MiB)", path, (unsigned long long)b->capacity,
             (unsigned long long)(sz >> 20));
    return RVM_OK;
}

void virtio_blk_close(virtio_blk *b) {
    if (!b)
        return;
    if (b->fd >= 0)
        close(b->fd);
    b->fd = -1;
}

virtio_backend *virtio_blk_backend(virtio_blk *b) {
    return b ? &b->be : NULL;
}
