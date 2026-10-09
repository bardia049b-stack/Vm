/*
 * virtio.h -- virtio-mmio (version 2) transport plus the split virtqueue.
 *
 * One transport implementation, many backends: block, net, gpu, input and
 * sound all plug in through virtio_backend and share the same ring code.
 * Descriptor chains are walked with plain guest-RAM copies -- no DMA engine,
 * no scatter-gather bookkeeping, which is exactly what keeps this small.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RVM_VIRTIO_H
#define RVM_VIRTIO_H

#include "../bus/bus.h"
#include "../rvm.h"

#define VIRTIO_MMIO_MAGIC 0x74726976u /* "virt" */
#define VIRTIO_VERSION    2
#define VIRTIO_VENDOR     0x52766Du /* "RvM" */

#define VIRTIO_MAX_QUEUES 4
#define VIRTQUEUE_NUM_MAX 256
#define VQ_MAX_IOV        16

/* virtqueue descriptor flags */
#define VRING_DESC_F_NEXT     1
#define VRING_DESC_F_WRITE    2
#define VRING_DESC_F_INDIRECT 4

/* STATUS bits */
#define VIRTIO_STATUS_ACK         1
#define VIRTIO_STATUS_DRIVER      2
#define VIRTIO_STATUS_DRIVER_OK   4
#define VIRTIO_STATUS_FEATURES_OK 8
#define VIRTIO_STATUS_FAILED      128

/* Device IDs */
#define VIRTIO_ID_NET     1
#define VIRTIO_ID_BLOCK   2
#define VIRTIO_ID_CONSOLE 3
#define VIRTIO_ID_RNG     4
#define VIRTIO_ID_BALLOON 5
#define VIRTIO_ID_RPMSG   7
#define VIRTIO_ID_SCSI    8
#define VIRTIO_ID_9P      9
#define VIRTIO_ID_GPU     16
#define VIRTIO_ID_INPUT   18
#define VIRTIO_ID_VSOCK   19
#define VIRTIO_ID_FS      26
#define VIRTIO_ID_SOUND   25

typedef struct vring_desc {
    u64 addr;
    u32 len;
    u16 flags;
    u16 next;
} vring_desc;

typedef struct virtqueue {
    u32 num_max;
    u32 num;
    bool ready;
    u64 desc_addr;
    u64 avail_addr;
    u64 used_addr;
    u32 last_avail;
    u64 n_notify, n_used;
} virtqueue;

typedef struct vq_iov {
    u64 addr; /* guest physical */
    u32 len;
    bool write; /* true = device writes here (guest buffer) */
} vq_iov;

typedef struct vq_chain {
    vq_iov iov[VQ_MAX_IOV];
    u32 n;
    u16 head;
    u32 total_len;
} vq_chain;

struct virtio;

typedef struct virtio_backend {
    u32 device_id;
    u32 nqueues;
    u32 config_len;
    u64 features; /* device feature bits */
    void *priv;

    void (*notify)(struct virtio *v, u32 qidx);
    void (*reset)(struct virtio *v);
    void (*read_config)(struct virtio *v, u32 off, u32 len, u8 *out);
    void (*write_config)(struct virtio *v, u32 off, u32 len, const u8 *in);
} virtio_backend;

typedef struct virtio {
    u32 features_sel, driver_sel;
    u64 device_features, driver_features;
    u32 queue_sel;
    u32 status;
    u32 irq_status;
    u32 irq;
    u32 generation;

    virtqueue q[VIRTIO_MAX_QUEUES];
    bus *bus;
    virtio_backend *be;

    void (*raise_irq)(void *ud, u32 irq, bool level);
    void *raise_ud;

    bool driver_ok;
} virtio;

rvm_err virtio_init(virtio *v, bus *b, virtio_backend *be, u32 irq);
bool virtio_load(void *dev, u64 off, u32 size, u64 *out);
bool virtio_store(void *dev, u64 off, u32 size, u64 val);
void virtio_set_irq(virtio *v, void (*raise)(void *ud, u32 irq, bool level), void *ud);

/*
 * Read handler for MMIO slots with no backend attached.  Reporting a valid
 * magic/version but DeviceID == 0 is how virtio-mmio signals "nothing here",
 * so Linux' probe simply skips the slot instead of faulting.
 */
bool virtio_absent_load(void *dev, u64 off, u32 size, u64 *out);

/* Queue helpers used by backends. */
bool vq_pop(virtio *v, u32 qidx, vq_chain *out); /* false = empty */
void vq_done(virtio *v, u32 qidx, const vq_chain *ch, u32 written);
void vq_interrupt(virtio *v);
bool vq_read(virtio *v, const vq_iov *iov, void *dst, u32 len);
bool vq_write(virtio *v, const vq_iov *iov, const void *src, u32 len);

/* Convenience: gather a chain's readable part into dst (returns bytes). */
u32 vq_gather_read(virtio *v, const vq_chain *ch, void *dst, u32 max);

#endif /* RVM_VIRTIO_H */
