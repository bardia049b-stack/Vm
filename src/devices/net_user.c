#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0
#endif
/*
#include <stdlib.h>
 * net_user.c -- userspace NAT for virtio-net (works without TAP / on Android).
 *
 * Guest: 10.0.2.15/24  GW: 10.0.2.2  DNS: 10.0.2.3
 *
 * SPDX-License-Identifier: MIT
 */
#include "net_user.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define ETH_HLEN 14
#define IP_HLEN  20
#define UDP_HLEN 8
#define TCP_HLEN 20
#define ETH_P_IP  0x0800
#define ETH_P_ARP 0x0806
#define MAX_TCP  24
#define MAX_UDP  16
#define RX_Q     48
#define RX_MAX   1600

static const u8 guest_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
static const u8 host_mac[6]  = {0x52, 0x55, 0x0a, 0x00, 0x02, 0x02};
static const u32 guest_ip = 0x0a00020f;
static const u32 gw_ip    = 0x0a000202;
static const u32 dns_ip   = 0x0a000203;

typedef struct {
    bool used;
    int fd;
    u16 gport, rport;
    u32 rip;
    u32 gseq, gack, hseq;
    u8 state; /* 0 free 1 connecting 2 est 3 closing */
} tconn;

typedef struct {
    bool used;
    int fd;
    u16 gport;
} uconn;

struct net_user {
    net_user_output_fn out;
    void *out_ud;
    tconn tcp[MAX_TCP];
    uconn udp[MAX_UDP];
    u8 rx[RX_Q][RX_MAX];
    u32 rx_len[RX_Q];
    u32 rx_r, rx_w;
};

static u16 csum_fold(u32 s) {
    while (s >> 16) s = (s & 0xffff) + (s >> 16);
    return (u16)~s;
}
static u16 ip_csum(const void *b, int n) {
    const u16 *p = b; u32 s = 0;
    while (n > 1) { s += *p++; n -= 2; }
    if (n) s += *(const u8 *)p;
    return csum_fold(s);
}
static u16 transport_csum(u32 src, u32 dst, u8 proto, const u8 *buf, int len) {
    u32 s = 0;
    s += (src >> 16) & 0xffff; s += src & 0xffff;
    s += (dst >> 16) & 0xffff; s += dst & 0xffff;
    s += htons(proto); s += htons((u16)len);
    const u16 *p = (const u16 *)buf;
    int n = len;
    while (n > 1) { s += *p++; n -= 2; }
    if (n) s += *(const u8 *)p;
    u16 r = csum_fold(s);
    return (proto == 17 && r == 0) ? 0xffff : r;
}

static void qrx(net_user *n, const u8 *f, u32 len) {
    if (len > RX_MAX) len = RX_MAX;
    u32 nx = (n->rx_w + 1) % RX_Q;
    if (nx == n->rx_r) return;
    memcpy(n->rx[n->rx_w], f, len);
    n->rx_len[n->rx_w] = len;
    n->rx_w = nx;
}
static void flush_rx(net_user *n) {
    while (n->rx_r != n->rx_w) {
        n->out(n->out_ud, n->rx[n->rx_r], n->rx_len[n->rx_r]);
        n->rx_r = (n->rx_r + 1) % RX_Q;
    }
}

static void eth_send(net_user *n, u16 type, const u8 *pay, u32 plen) {
    u8 f[RX_MAX];
    if (ETH_HLEN + plen > RX_MAX) return;
    memcpy(f, guest_mac, 6);
    memcpy(f + 6, host_mac, 6);
    f[12] = (u8)(type >> 8); f[13] = (u8)type;
    memcpy(f + ETH_HLEN, pay, plen);
    qrx(n, f, ETH_HLEN + plen);
}

static void ip_send_from(net_user *n, u32 src, u32 dst, u8 proto, const u8 *pay, u32 plen) {
    u8 p[RX_MAX];
    if (IP_HLEN + plen > RX_MAX - ETH_HLEN) return;
    memset(p, 0, IP_HLEN);
    p[0] = 0x45;
    u16 tot = (u16)(IP_HLEN + plen);
    p[2] = (u8)(tot >> 8); p[3] = (u8)tot;
    p[8] = 64; p[9] = proto;
    p[12] = (u8)(src >> 24); p[13] = (u8)(src >> 16);
    p[14] = (u8)(src >> 8);  p[15] = (u8)src;
    p[16] = (u8)(dst >> 24); p[17] = (u8)(dst >> 16);
    p[18] = (u8)(dst >> 8);  p[19] = (u8)dst;
    u16 c = ip_csum(p, IP_HLEN);
    p[10] = (u8)(c >> 8); p[11] = (u8)c;
    memcpy(p + IP_HLEN, pay, plen);
    eth_send(n, ETH_P_IP, p, IP_HLEN + plen);
}

static void handle_arp(net_user *n, const u8 *eth, u32 len) {
    if (len < ETH_HLEN + 28) return;
    const u8 *a = eth + ETH_HLEN;
    if (((a[6] << 8) | a[7]) != 1) return;
    u32 tpa = ((u32)a[24] << 24) | ((u32)a[25] << 16) | ((u32)a[26] << 8) | a[27];
    if (tpa != gw_ip && tpa != dns_ip) return;
    u8 r[28]; memset(r, 0, 28);
    r[1] = 1; r[2] = 8; r[4] = 6; r[5] = 4; r[7] = 2;
    memcpy(r + 8, host_mac, 6);
    r[14] = (u8)(tpa >> 24); r[15] = (u8)(tpa >> 16);
    r[16] = (u8)(tpa >> 8);  r[17] = (u8)tpa;
    memcpy(r + 18, eth + 6, 6);
    memcpy(r + 24, a + 14, 4);
    eth_send(n, ETH_P_ARP, r, 28);
}

static void dhcp_reply(net_user *n, const u8 *req, u32 len, u16 sport, u8 type) {
    if (len < 240) return;
    u8 out[300]; memset(out, 0, sizeof(out));
    out[0] = 2; out[1] = 1; out[2] = 6;
    memcpy(out + 4, req + 4, 4);
    out[16] = 10; out[17] = 0; out[18] = 2; out[19] = 15;
    out[20] = 10; out[21] = 0; out[22] = 2; out[23] = 2;
    memcpy(out + 28, req + 28, 16);
    out[236] = 99; out[237] = 130; out[238] = 83; out[239] = 99;
    u32 o = 240;
    out[o++] = 53; out[o++] = 1; out[o++] = type;
    out[o++] = 1; out[o++] = 4; out[o++] = 255; out[o++] = 255; out[o++] = 255; out[o++] = 0;
    out[o++] = 3; out[o++] = 4; out[o++] = 10; out[o++] = 0; out[o++] = 2; out[o++] = 2;
    out[o++] = 6; out[o++] = 4; out[o++] = 10; out[o++] = 0; out[o++] = 2; out[o++] = 3;
    out[o++] = 51; out[o++] = 4; out[o++] = 0; out[o++] = 1; out[o++] = 0x51; out[o++] = 0x80;
    out[o++] = 54; out[o++] = 4; out[o++] = 10; out[o++] = 0; out[o++] = 2; out[o++] = 2;
    out[o++] = 0xff;
    u8 udp[8 + 300]; memset(udp, 0, 8);
    udp[1] = 67; udp[2] = (u8)(sport >> 8); udp[3] = (u8)sport;
    u16 ul = (u16)(8 + o);
    udp[4] = (u8)(ul >> 8); udp[5] = (u8)ul;
    memcpy(udp + 8, out, o);
    u16 uc = transport_csum(gw_ip, guest_ip, 17, udp, ul);
    udp[6] = (u8)(uc >> 8); udp[7] = (u8)uc;
    ip_send_from(n, gw_ip, guest_ip, 17, udp, ul);
}

static void handle_dns(net_user *n, const u8 *q, u32 ql, u16 sport) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return;
    struct timeval tv = {2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET; sa.sin_port = htons(53);
    sa.sin_addr.s_addr = htonl(0x08080808);
    if (sendto(fd, q, ql, 0, (struct sockaddr *)&sa, sizeof(sa)) < 0) { close(fd); return; }
    u8 resp[512];
    ssize_t nr = recvfrom(fd, resp, sizeof(resp), 0, NULL, NULL);
    close(fd);
    if (nr <= 0) return;
    u8 udp[8 + 512]; memset(udp, 0, 8);
    udp[1] = 53; udp[2] = (u8)(sport >> 8); udp[3] = (u8)sport;
    u16 ul = (u16)(8 + nr);
    udp[4] = (u8)(ul >> 8); udp[5] = (u8)ul;
    memcpy(udp + 8, resp, (size_t)nr);
    u16 uc = transport_csum(dns_ip, guest_ip, 17, udp, ul);
    udp[6] = (u8)(uc >> 8); udp[7] = (u8)uc;
    ip_send_from(n, dns_ip, guest_ip, 17, udp, ul);
}

static void handle_udp(net_user *n, u32 dst, const u8 *udp, u32 len) {
    if (len < 8) return;
    u16 sp = (u16)((udp[0] << 8) | udp[1]);
    u16 dp = (u16)((udp[2] << 8) | udp[3]);
    const u8 *pl = udp + 8; u32 plen = len - 8;
    if (dp == 67) {
        u8 mt = 0;
        for (u32 i = 240; i + 1 < plen; ) {
            u8 c = pl[i++]; if (c == 255) break; if (c == 0) continue;
            u8 l = pl[i++]; if (i + l > plen) break;
            if (c == 53 && l) mt = pl[i];
            i += l;
        }
        dhcp_reply(n, pl, plen, sp, mt == 1 ? 2 : 5);
        return;
    }
    if (dst == dns_ip && dp == 53) { handle_dns(n, pl, plen, sp); return; }
    int slot = -1;
    for (int i = 0; i < MAX_UDP; i++) {
        if (n->udp[i].used && n->udp[i].gport == sp) { slot = i; break; }
        if (!n->udp[i].used && slot < 0) slot = i;
    }
    if (slot < 0) return;
    uconn *c = &n->udp[slot];
    if (!c->used) {
        int fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (fd < 0) return;
        c->used = true; c->fd = fd; c->gport = sp;
    }
    struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET; sa.sin_port = htons(dp); sa.sin_addr.s_addr = htonl(dst);
    sendto(c->fd, pl, plen, 0, (struct sockaddr *)&sa, sizeof(sa));
}

static void tcp_emit(net_user *n, tconn *c, u8 flags, const u8 *data, u32 dlen) {
    u8 seg[RX_MAX]; memset(seg, 0, TCP_HLEN);
    seg[0] = (u8)(c->rport >> 8); seg[1] = (u8)c->rport;
    seg[2] = (u8)(c->gport >> 8); seg[3] = (u8)c->gport;
    seg[4] = (u8)(c->hseq >> 24); seg[5] = (u8)(c->hseq >> 16);
    seg[6] = (u8)(c->hseq >> 8);  seg[7] = (u8)c->hseq;
    seg[8] = (u8)(c->gack >> 24); seg[9] = (u8)(c->gack >> 16);
    seg[10] = (u8)(c->gack >> 8); seg[11] = (u8)c->gack;
    seg[12] = 0x50; seg[13] = flags; seg[14] = 0xff; seg[15] = 0xff;
    if (data && dlen) memcpy(seg + TCP_HLEN, data, dlen);
    u32 sl = TCP_HLEN + dlen;
    u16 tc = transport_csum(c->rip, guest_ip, 6, seg, (int)sl);
    seg[16] = (u8)(tc >> 8); seg[17] = (u8)tc;
    ip_send_from(n, c->rip, guest_ip, 6, seg, sl);
}

static void handle_tcp(net_user *n, u32 dst, const u8 *tcp, u32 len) {
    if (len < TCP_HLEN) return;
    u16 sp = (u16)((tcp[0] << 8) | tcp[1]);
    u16 dp = (u16)((tcp[2] << 8) | tcp[3]);
    u32 seq = ((u32)tcp[4] << 24) | ((u32)tcp[5] << 16) | ((u32)tcp[6] << 8) | tcp[7];
    u8 doff = (u8)((tcp[12] >> 4) * 4);
    u8 flags = tcp[13];
    if (doff > len) return;
    const u8 *data = tcp + doff; u32 dlen = len - doff;

    tconn *c = NULL;
    for (int i = 0; i < MAX_TCP; i++)
        if (n->tcp[i].used && n->tcp[i].gport == sp) { c = &n->tcp[i]; break; }

    if (flags & 0x02) { /* SYN */
        if (!c) {
            for (int i = 0; i < MAX_TCP; i++) if (!n->tcp[i].used) { c = &n->tcp[i]; break; }
        }
        if (!c) return;
        if (c->fd >= 0) close(c->fd);
        memset(c, 0, sizeof(*c));
        c->used = true; c->fd = -1; c->gport = sp; c->rport = dp; c->rip = dst;
        c->gack = seq + 1; c->hseq = 1000; c->state = 1;
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { c->used = false; return; }
        c->fd = fd;
        struct sockaddr_in sa; memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET; sa.sin_port = htons(dp); sa.sin_addr.s_addr = htonl(dst);
        int r = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
        if (r == 0 || errno == EINPROGRESS) {
            /* SYN-ACK immediately so guest proceeds; data waits for connect complete */
            tcp_emit(n, c, 0x12, NULL, 0);
            c->hseq++;
            if (r == 0) c->state = 2;
        } else {
            close(fd); c->fd = -1; c->used = false;
        }
        return;
    }
    if (!c || c->fd < 0) return;
    c->gack = seq + dlen + ((flags & 1) ? 1 : 0);
    if (dlen) {
        send(c->fd, data, dlen, MSG_NOSIGNAL);
        tcp_emit(n, c, 0x10, NULL, 0);
    }
    if (flags & 1) {
        shutdown(c->fd, SHUT_WR);
        tcp_emit(n, c, 0x11, NULL, 0);
        c->hseq++;
        c->state = 3;
    }
}

static void handle_icmp(net_user *n, u32 src, const u8 *icmp, u32 len) {
    if (len < 8 || icmp[0] != 8) return;
    u8 r[RX_MAX]; if (len > sizeof(r)) return;
    memcpy(r, icmp, len); r[0] = 0; r[2] = r[3] = 0;
    u16 c = ip_csum(r, (int)len); r[2] = (u8)(c >> 8); r[3] = (u8)c;
    ip_send_from(n, gw_ip, src, 1, r, len);
}

static void handle_ip(net_user *n, const u8 *eth, u32 len) {
    if (len < ETH_HLEN + IP_HLEN) return;
    const u8 *ip = eth + ETH_HLEN;
    if ((ip[0] >> 4) != 4) return;
    u32 ihl = (ip[0] & 0xf) * 4;
    u16 tot = (u16)((ip[2] << 8) | ip[3]);
    if ((u32)(ETH_HLEN + tot) > len) tot = (u16)(len - ETH_HLEN);
    if (tot < ihl) return;
    u32 src = ((u32)ip[12] << 24) | ((u32)ip[13] << 16) | ((u32)ip[14] << 8) | ip[15];
    u32 dst = ((u32)ip[16] << 24) | ((u32)ip[17] << 16) | ((u32)ip[18] << 8) | ip[19];
    u8 proto = ip[9];
    const u8 *pl = ip + ihl; u32 plen = tot - ihl;
    if (proto == 1) handle_icmp(n, src, pl, plen);
    else if (proto == 17) handle_udp(n, dst, pl, plen);
    else if (proto == 6) handle_tcp(n, dst, pl, plen);
}

rvm_err net_user_init(net_user **out, net_user_output_fn fn, void *ud) {
    net_user *n = calloc(1, sizeof(*n));
    if (!n) return RVM_ERR_NOMEM;
    n->out = fn; n->out_ud = ud;
    for (int i = 0; i < MAX_TCP; i++) n->tcp[i].fd = -1;
    for (int i = 0; i < MAX_UDP; i++) n->udp[i].fd = -1;
    *out = n;
    return RVM_OK;
}

void net_user_free(net_user *n) {
    if (!n) return;
    for (int i = 0; i < MAX_TCP; i++) if (n->tcp[i].fd >= 0) close(n->tcp[i].fd);
    for (int i = 0; i < MAX_UDP; i++) if (n->udp[i].fd >= 0) close(n->udp[i].fd);
    free(n);
}

void net_user_input(net_user *n, const u8 *frame, u32 len) {
    if (!n || len < ETH_HLEN) return;
    u16 t = (u16)((frame[12] << 8) | frame[13]);
    if (t == ETH_P_ARP) handle_arp(n, frame, len);
    else if (t == ETH_P_IP) handle_ip(n, frame, len);
    flush_rx(n);
}

void net_user_poll(net_user *n) {
    if (!n) return;
    for (int i = 0; i < MAX_UDP; i++) {
        uconn *c = &n->udp[i];
        if (!c->used || c->fd < 0) continue;
        u8 buf[1500]; struct sockaddr_in sa; socklen_t sl = sizeof(sa);
        ssize_t r = recvfrom(c->fd, buf, sizeof(buf), 0, (struct sockaddr *)&sa, &sl);
        if (r <= 0) continue;
        u8 udp[8 + 1500]; memset(udp, 0, 8);
        u16 sp = ntohs(sa.sin_port);
        udp[0] = (u8)(sp >> 8); udp[1] = (u8)sp;
        udp[2] = (u8)(c->gport >> 8); udp[3] = (u8)c->gport;
        u16 ul = (u16)(8 + r);
        udp[4] = (u8)(ul >> 8); udp[5] = (u8)ul;
        memcpy(udp + 8, buf, (size_t)r);
        u32 remote = ntohl(sa.sin_addr.s_addr);
        u16 uc = transport_csum(remote, guest_ip, 17, udp, ul);
        udp[6] = (u8)(uc >> 8); udp[7] = (u8)uc;
        ip_send_from(n, remote, guest_ip, 17, udp, ul);
    }
    for (int i = 0; i < MAX_TCP; i++) {
        tconn *c = &n->tcp[i];
        if (!c->used || c->fd < 0) continue;
        if (c->state == 1) {
            int err = 0; socklen_t el = sizeof(err);
            if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el) == 0 && err == 0)
                c->state = 2;
            else if (err != 0) {
                tcp_emit(n, c, 0x14, NULL, 0); /* RST */
                close(c->fd); c->fd = -1; c->used = false;
                continue;
            }
        }
        if (c->state >= 2) {
            u8 buf[1400];
            ssize_t r = recv(c->fd, buf, sizeof(buf), 0);
            if (r > 0) {
                tcp_emit(n, c, 0x18, buf, (u32)r); /* PSH+ACK */
                c->hseq += (u32)r;
            } else if (r == 0) {
                tcp_emit(n, c, 0x11, NULL, 0);
                c->hseq++;
                close(c->fd); c->fd = -1; c->used = false;
            }
        }
    }
    flush_rx(n);
}
