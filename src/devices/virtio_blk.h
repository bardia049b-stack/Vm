/*
 * virtio_blk.h -- virtio block device backed by a regular host file.
 * SPDX-License-Identifier: MIT
 */
#ifndef RVM_VIRTIO_BLK_H
#define RVM_VIRTIO_BLK_H

#include "virtio.h"

#define VIRTIO_BLK_SECTOR_SIZE 512

#define VIRTIO_BLK_T_IN     0 /* device -> guest (read)  */
#define VIRTIO_BLK_T_OUT    1 /* guest -> device (write) */
#define VIRTIO_BLK_T_FLUSH  4
#define VIRTIO_BLK_T_GET_ID 8

#define VIRTIO_BLK_S_OK     0
#define VIRTIO_BLK_S_IOERR  1
#define VIRTIO_BLK_S_UNSUPP 2

/* Feature bit numbers from the virtio specification. */
#define VIRTIO_BLK_F_BLK_SIZE 6
#define VIRTIO_BLK_F_FLUSH    9
#define VIRTIO_F_VERSION_1    32

typedef struct virtio_blk {
    virtio_backend be; /* must be first: vm.c casts device <-> backend */
    int fd;
    u64 capacity; /* in 512-byte sectors */
    u32 blk_size;
    bool readonly;
    char id[21];

    u64 n_read, n_write, n_flush, n_err;
} virtio_blk;

/*
 * Open `path` (created if missing and `create` is set, sized to `bytes`).
 * The returned backend must be passed to virtio_init() with VIRTIO_ID_BLOCK.
 */
rvm_err virtio_blk_open(virtio_blk *b, const char *path, u64 bytes, bool create);
void virtio_blk_close(virtio_blk *b);
virtio_backend *virtio_blk_backend(virtio_blk *b);

#endif /* RVM_VIRTIO_BLK_H */
