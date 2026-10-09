/* SPDX-License-Identifier: MIT */
#include "virtio.h"

/* Register map (offsets within the 4 KiB MMIO window) */
#define REG_MAGIC               0x000
#define REG_VERSION             0x004
#define REG_DEVICE_ID           0x008
#define REG_VENDOR_ID           0x00C
#define REG_DEVICE_FEATURES     0x010
#define REG_DEVICE_FEATURES_SEL 0x014
#define REG_DRIVER_FEATURES     0x020
#define REG_DRIVER_FEATURES_SEL 0x024
#define REG_QUEUE_SEL           0x030
#define REG_QUEUE_NUM_MAX       0x034
#define REG_QUEUE_NUM           0x038
#define REG_QUEUE_READY         0x044
#define REG_QUEUE_NOTIFY        0x050
#define REG_INTERRUPT_STATUS    0x060
#define REG_INTERRUPT_ACK       0x064
#define REG_STATUS              0x070
#define REG_QUEUE_DESC_LO       0x080
#define REG_QUEUE_DESC_HI       0x084
#define REG_QUEUE_DRIVER_LO     0x090
#define REG_QUEUE_DRIVER_HI     0x094
#define REG_QUEUE_DEVICE_LO     0x0A0
#define REG_QUEUE_DEVICE_HI     0x0A4
#define REG_CONFIG_GEN          0x0FC
#define REG_CONFIG              0x100

rvm_err virtio_init(virtio *v, bus *b, virtio_backend *be, u32 irq) {
    if (!v || !b || !be)
        return RVM_ERR_BADARG;
    memset(v, 0, sizeof(*v));
    v->bus = b;
    v->be = be;
    v->irq = irq;
    v->device_features = be->features;
    for (u32 i = 0; i < VIRTIO_MAX_QUEUES; i++) {
        v->q[i].num_max = VIRTQUEUE_NUM_MAX;
        v->q[i].num = VIRTQUEUE_NUM_MAX;
    }
    return RVM_OK;
}

void virtio_set_irq(virtio *v, void (*raise)(void *ud, u32 irq, bool level), void *ud) {
    v->raise_irq = raise;
    v->raise_ud = ud;
}

static void virtio_reset(virtio *v) {
    virtio_backend *be = v->be;
    bus *b = v->bus;
    void (*raise)(void *, u32, bool) = v->raise_irq;
    void *raise_ud = v->raise_ud;
    u32 irq = v->irq;

    memset(v, 0, sizeof(*v));
    v->be = be;
    v->bus = b;
    v->irq = irq;
    v->raise_irq = raise;
    v->raise_ud = raise_ud;
    v->device_features = be->features;
    for (u32 i = 0; i < VIRTIO_MAX_QUEUES; i++) {
        v->q[i].num_max = VIRTQUEUE_NUM_MAX;
        v->q[i].num = VIRTQUEUE_NUM_MAX;
    }
    if (be->reset)
        be->reset(v);
    if (raise)
        raise(raise_ud, irq, false);
}

static virtqueue *cur_queue(virtio *v) {
    if (v->queue_sel >= RVM_ARRAY_SIZE(v->q))
        return NULL;
    return &v->q[v->queue_sel];
}

bool virtio_load(void *dev, u64 off, u32 size, u64 *out) {
    virtio *v = (virtio *)dev;
    if (size != 4)
        return false;
    *out = 0;

    if (off >= REG_CONFIG) {
        u32 coff = (u32)(off - REG_CONFIG);
        u8 buf[8] = {0};
        u32 len = (size == 4) ? 4 : 8;
        if (v->be->read_config)
            v->be->read_config(v, coff, len, buf);
        memcpy(out, buf, len);
        return true;
    }

    switch (off) {
    case REG_MAGIC:
        *out = VIRTIO_MMIO_MAGIC;
        break;
    case REG_VERSION:
        *out = VIRTIO_VERSION;
        break;
    case REG_DEVICE_ID:
        *out = v->be->device_id;
        break;
    case REG_VENDOR_ID:
        *out = VIRTIO_VENDOR;
        break;
    case REG_DEVICE_FEATURES:
        *out = (u32)((v->device_features >> (32 * (v->features_sel & 1))) & 0xFFFFFFFFu);
        break;
    case REG_QUEUE_NUM_MAX:
        *out = cur_queue(v) ? cur_queue(v)->num_max : 0;
        break;
    case REG_QUEUE_READY:
        *out = cur_queue(v) ? (cur_queue(v)->ready ? 1 : 0) : 0;
        break;
    case REG_INTERRUPT_STATUS:
        *out = v->irq_status;
        break;
    case REG_STATUS:
        *out = v->status;
        break;
    case REG_CONFIG_GEN:
        *out = v->generation;
        break;
    default:
        break; /* reserved registers read as zero */
    }
    return true;
}

bool virtio_store(void *dev, u64 off, u32 size, u64 val) {
    virtio *v = (virtio *)dev;
    if (size != 4)
        return false;
    u32 w = (u32)val;

    if (off >= REG_CONFIG) {
        u32 coff = (u32)(off - REG_CONFIG);
        u8 buf[4];
        memcpy(buf, &w, 4);
        if (v->be->write_config)
            v->be->write_config(v, coff, 4, buf);
        return true;
    }

    switch (off) {
    case REG_DEVICE_FEATURES_SEL:
        v->features_sel = w;
        break;
    case REG_DRIVER_FEATURES_SEL:
        v->driver_sel = w;
        break;
    case REG_DRIVER_FEATURES: {
        u64 mask = 0xFFFFFFFFULL << (32 * (v->driver_sel & 1));
        v->driver_features = (v->driver_features & ~mask) | ((u64)w << (32 * (v->driver_sel & 1)));
        break;
    }
    case REG_QUEUE_SEL:
        v->queue_sel = w;
        break;
    case REG_QUEUE_NUM:
        if (cur_queue(v))
            cur_queue(v)->num = w;
        break;
    case REG_QUEUE_READY: {
        virtqueue *q = cur_queue(v);
        if (q) {
            q->ready = (w & 1) != 0;
            if (q->ready)
                LOG_TRACE("virtio%u: queue %u ready (num=%u)", v->be->device_id, v->queue_sel,
                          q->num);
        }
        break;
    }
    case REG_QUEUE_NOTIFY: {
        virtqueue *q = cur_queue(v);
        if (q)
            q->n_notify++;
        if (v->driver_ok && v->be->notify)
            v->be->notify(v, w);
        break;
    }
    case REG_INTERRUPT_ACK:
        v->irq_status &= ~w;
        if (v->irq_status == 0 && v->raise_irq)
            v->raise_irq(v->raise_ud, v->irq, false);
        break;
    case REG_STATUS:
        if (w == 0) {
            virtio_reset(v);
            break;
        }
        v->status = w & 0xFF;
        if (w & VIRTIO_STATUS_FEATURES_OK) {
            /* Refuse a feature set we do not implement. */
            if ((v->driver_features & ~v->device_features) != 0) {
                LOG_WARN("virtio%u: driver asked for unsupported features 0x%llx", v->be->device_id,
                         (unsigned long long)(v->driver_features & ~v->device_features));
                v->status &= ~VIRTIO_STATUS_FEATURES_OK;
            }
        }
        if (w & VIRTIO_STATUS_DRIVER_OK) {
            v->driver_ok = true;
            LOG_INFO("virtio%u: DRIVER_OK, device is live", v->be->device_id);
        }
        break;

    case REG_QUEUE_DESC_LO:
        if (cur_queue(v))
            cur_queue(v)->desc_addr = (cur_queue(v)->desc_addr & ~0xFFFFFFFFULL) | w;
        break;
    case REG_QUEUE_DESC_HI:
        if (cur_queue(v))
            cur_queue(v)->desc_addr = (cur_queue(v)->desc_addr & 0xFFFFFFFFULL) | ((u64)w << 32);
        break;
    case REG_QUEUE_DRIVER_LO:
        if (cur_queue(v))
            cur_queue(v)->avail_addr = (cur_queue(v)->avail_addr & ~0xFFFFFFFFULL) | w;
        break;
    case REG_QUEUE_DRIVER_HI:
        if (cur_queue(v))
            cur_queue(v)->avail_addr = (cur_queue(v)->avail_addr & 0xFFFFFFFFULL) | ((u64)w << 32);
        break;
    case REG_QUEUE_DEVICE_LO:
        if (cur_queue(v))
            cur_queue(v)->used_addr = (cur_queue(v)->used_addr & ~0xFFFFFFFFULL) | w;
        break;
    case REG_QUEUE_DEVICE_HI:
        if (cur_queue(v))
            cur_queue(v)->used_addr = (cur_queue(v)->used_addr & 0xFFFFFFFFULL) | ((u64)w << 32);
        break;
    default:
        break;
    }
    return true;
}

/* ---------------------------------------------------------- ring helpers */

static bool ring_load16(virtio *v, u64 addr, u16 *out) {
    u64 t = 0;
    if (!bus_load(v->bus, addr, 2, &t))
        return false;
    *out = (u16)t;
    return true;
}
static bool ring_store16(virtio *v, u64 addr, u16 val) {
    return bus_store(v->bus, addr, 2, val);
}
static bool ring_store32(virtio *v, u64 addr, u32 val) {
    return bus_store(v->bus, addr, 4, val);
}

void vq_interrupt(virtio *v) {
    v->irq_status |= 1;
    if (v->raise_irq)
        v->raise_irq(v->raise_ud, v->irq, true);
}

bool vq_pop(virtio *v, u32 qidx, vq_chain *out) {
    if (qidx >= RVM_ARRAY_SIZE(v->q))
        return false;
    virtqueue *q = &v->q[qidx];
    if (!q->ready || q->num == 0)
        return false;

    u16 avail_idx = 0;
    if (!ring_load16(v, q->avail_addr + 2, &avail_idx))
        return false;
    if ((u16)q->last_avail == avail_idx)
        return false; /* ring empty */

    u16 head = 0;
    u16 slot = (u16)(q->last_avail % q->num);
    if (!ring_load16(v, q->avail_addr + 4 + 2 * slot, &head))
        return false;
    q->last_avail = (u16)(q->last_avail + 1);

    memset(out, 0, sizeof(*out));
    out->head = head;

    u16 idx = head;
    for (u32 guard = 0; guard < VQ_MAX_IOV; guard++) {
        vring_desc d;
        if (!bus_read_bytes(v->bus, q->desc_addr + (u64)idx * sizeof(vring_desc), &d, sizeof(d)))
            return false;
        if (out->n < VQ_MAX_IOV) {
            vq_iov *e = &out->iov[out->n++];
            e->addr = d.addr;
            e->len = d.len;
            e->write = (d.flags & VRING_DESC_F_WRITE) != 0;
            out->total_len += d.len;
        }
        if (!(d.flags & VRING_DESC_F_NEXT))
            break;
        idx = d.next;
    }
    return out->n > 0;
}

void vq_done(virtio *v, u32 qidx, const vq_chain *ch, u32 written) {
    if (qidx >= RVM_ARRAY_SIZE(v->q))
        return;
    virtqueue *q = &v->q[qidx];
    u16 used_idx = 0;
    if (!ring_load16(v, q->used_addr + 2, &used_idx))
        return;
    u16 slot = (u16)(used_idx % q->num);
    u64 elem = q->used_addr + 4 + 8 * (u64)slot;
    ring_store32(v, elem + 0, ch->head);
    ring_store32(v, elem + 4, written);
    ring_store16(v, q->used_addr + 2, (u16)(used_idx + 1));
    q->n_used++;
    vq_interrupt(v);
}

bool vq_read(virtio *v, const vq_iov *iov, void *dst, u32 len) {
    return bus_read_bytes(v->bus, iov->addr, dst, len);
}
bool vq_write(virtio *v, const vq_iov *iov, const void *src, u32 len) {
    return bus_write_bytes(v->bus, iov->addr, src, len);
}

u32 vq_gather_read(virtio *v, const vq_chain *ch, void *dst, u32 max) {
    u8 *p = (u8 *)dst;
    u32 done = 0;
    for (u32 i = 0; i < ch->n && done < max; i++) {
        if (ch->iov[i].write)
            continue; /* device-writable buffers hold no input */
        u32 take = RVM_MIN(ch->iov[i].len, max - done);
        if (!bus_read_bytes(v->bus, ch->iov[i].addr, p + done, take))
            break;
        done += take;
    }
    return done;
}

bool virtio_absent_load(void *dev, u64 off, u32 size, u64 *out) {
    RVM_UNUSED(dev);
    if (size != 4)
        return false;
    switch (off) {
    case REG_MAGIC:
        *out = VIRTIO_MMIO_MAGIC;
        break;
    case REG_VERSION:
        *out = VIRTIO_VERSION;
        break;
    case REG_VENDOR_ID:
        *out = VIRTIO_VENDOR;
        break;
    default:
        *out = 0;
        break; /* DEVICE_ID == 0 -> slot is empty */
    }
    return true;
}
