/*
 * virtio_net.c -- virtio-net device + userspace backend.
 *
 * SPDX-License-Identifier: MIT
 */
#include "virtio_net.h"
#include "net_user.h"
#include "../bus/bus.h"

#include <string.h>

#define VIRTIO_F_VERSION_1 32
#define Q_RX 0
#define Q_TX 1
#define RX_MAX_PKT 1600

static void store_bytes(bus *b, u64 addr, const u8 *p, u32 n) {
    for (u32 i = 0; i < n; i++)
        bus_store(b, addr + i, 1, p[i]);
}

static void net_output_cb(void *ud, const u8 *frame, u32 len) {
    virtio_net *n = (virtio_net *)ud;
    if (!n->vio || !n->vio->bus) return;
    vq_chain ch;
    if (!vq_pop(n->vio, Q_RX, &ch)) {
        n->n_drop++;
        return;
    }
    u8 hdr[VIRTIO_NET_HDR_SIZE];
    memset(hdr, 0, sizeof(hdr));
    u32 need = VIRTIO_NET_HDR_SIZE + len;
    u32 written = 0;
    const u8 *src = hdr;
    u32 src_left = VIRTIO_NET_HDR_SIZE;
    bool in_frame = false;
    for (u32 i = 0; i < ch.n && written < need; i++) {
        if (!ch.iov[i].write) continue;
        u32 pos = 0;
        while (pos < ch.iov[i].len && written < need) {
            if (!in_frame && src_left == 0) {
                src = frame;
                src_left = len;
                in_frame = true;
            }
            if (src_left == 0) break;
            u32 take = ch.iov[i].len - pos;
            if (take > src_left) take = src_left;
            store_bytes(n->vio->bus, ch.iov[i].addr + pos, src, take);
            pos += take;
            src += take;
            src_left -= take;
            written += take;
        }
    }
    vq_done(n->vio, Q_RX, &ch, written);
    n->n_rx++;
}

static void net_notify(virtio *v, u32 qidx) {
    virtio_net *n = (virtio_net *)v->be->priv;
    if (qidx != Q_TX) {
        if (n->user) net_user_poll((net_user *)n->user);
        return;
    }
    vq_chain ch;
    while (vq_pop(v, Q_TX, &ch)) {
        u8 pkt[RX_MAX_PKT];
        u32 got = vq_gather_read(v, &ch, pkt, sizeof(pkt));
        if (got > VIRTIO_NET_HDR_SIZE && n->user) {
            net_user_input((net_user *)n->user, pkt + VIRTIO_NET_HDR_SIZE, got - VIRTIO_NET_HDR_SIZE);
            n->n_tx++;
        }
        vq_done(v, Q_TX, &ch, 0);
    }
}

static void net_reset(virtio *v) { (void)v; }

static void net_read_config(virtio *v, u32 off, u32 len, u8 *out) {
    virtio_net *n = (virtio_net *)v->be->priv;
    u8 cfg[8];
    memcpy(cfg, n->mac, 6);
    cfg[6] = (u8)(n->status & 0xff);
    cfg[7] = (u8)(n->status >> 8);
    memset(out, 0, len);
    if (off < sizeof(cfg)) {
        u32 ncopy = len;
        if (off + ncopy > sizeof(cfg)) ncopy = (u32)sizeof(cfg) - off;
        memcpy(out, cfg + off, ncopy);
    }
}

static void net_write_config(virtio *v, u32 off, u32 len, const u8 *in) {
    (void)v; (void)off; (void)len; (void)in;
}

rvm_err virtio_net_init(virtio_net *n) {
    if (!n) return RVM_ERR_BADARG;
    memset(n, 0, sizeof(*n));
    n->mac[0] = 0x52; n->mac[1] = 0x54; n->mac[2] = 0x00;
    n->mac[3] = 0x12; n->mac[4] = 0x34; n->mac[5] = 0x56;
    n->status = VIRTIO_NET_S_LINK_UP;
    n->be.device_id = VIRTIO_ID_NET;
    n->be.nqueues = 2;
    n->be.config_len = 8;
    n->be.features = (1ULL << VIRTIO_NET_F_MAC) | (1ULL << VIRTIO_NET_F_STATUS) |
                     (1ULL << VIRTIO_F_VERSION_1);
    n->be.priv = n;
    n->be.notify = net_notify;
    n->be.reset = net_reset;
    n->be.read_config = net_read_config;
    n->be.write_config = net_write_config;
    return net_user_init((net_user **)&n->user, net_output_cb, n);
}

void virtio_net_shutdown(virtio_net *n) {
    if (!n) return;
    net_user_free((net_user *)n->user);
    n->user = NULL;
}

virtio_backend *virtio_net_backend(virtio_net *n) { return n ? &n->be : NULL; }

void virtio_net_poll(virtio_net *n) {
    if (n && n->user) net_user_poll((net_user *)n->user);
}

u64 virtio_net_rx_packets(const virtio_net *n) { return n ? n->n_rx : 0; }
u64 virtio_net_tx_packets(const virtio_net *n) { return n ? n->n_tx : 0; }
