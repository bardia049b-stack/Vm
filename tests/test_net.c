/* SPDX-License-Identifier: MIT */
/*
 * test_net.c -- the userspace NAT relay, driven the way a guest drives it.
 *
 * The bug this suite exists for: the relay handed the guest whatever recv()
 * returned and then forgot about it.  When the device had no RX descriptor
 * ready, or when the queue in front of it was full, the frame was lost, and
 * with nothing to re-send from the stream stalled forever.  Small transfers
 * (a DNS answer, a 22 KB Release file) were fine, so everything looked
 * healthy until apt tried to pull a few megabytes.
 *
 * So: push 120 KB through the relay from a real loopback listener while the
 * fake guest refuses some frames and silently loses some segments.  The only
 * way the bytes can arrive in order and complete is flow control plus
 * retransmission, which is precisely what used to be missing.
 */
#include "test.h"
#include "../src/devices/net_user.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define GUEST_IP    0x0a00020fu /* 10.0.2.15, the address the NAT serves */
#define GUEST_PORT  40000u
#define TOTAL       120000u
/* Now and then the device has no RX descriptor posted and refuses a frame,
 * which stalls everything queued behind it.  Refusing one in forty is enough to
 * prove the relay holds that data and re-offers it, instead of losing it. */
#define REFUSE_EVERY 40u
#define REFUSE_AT     7u
#define LOSE_EVERY  40u  /* and sometimes a frame disappears inside the guest */

static u8 g_pattern[TOTAL];
static u8 g_got[TOTAL];

typedef struct {
    net_user *n;
    int acc; /* the upstream side, already connected */
    u16 rport;
    u32 gseq;      /* the next seq number we expect from the relay */
    u32 first;     /* that seq as of the handshake, so seq maps to an offset */
    u32 payload;   /* bytes stored */
    u32 fills;     /* frames the device was asked to take */
    u32 refuses;   /* ...and the three times it had no buffer ready */
    u32 lost_seg;  /* segments we made the guest never see */
    u32 refused;   /* the ones it said no to */
    u32 dropped;   /* data segments we were offered at all */
    bool started;
} fake_guest;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + 1e-9 * (double)ts.tv_nsec;
}

static u16 csum16(const u8 *b, int n) {
    u32 s = 0;
    while (n > 1) {
        s += ((u32)b[0] << 8) | b[1];
        b += 2;
        n -= 2;
    }
    if (n)
        s += (u32)b[0] << 8;
    while (s >> 16)
        s = (s & 0xffff) + (s >> 16);
    return (u16)~s;
}

static void put16(u8 *p, u16 v) {
    p[0] = (u8)(v >> 8);
    p[1] = (u8)v;
}
static void put32(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
}
static u32 get32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* The relay listens to us the way it listens to a guest: Ethernet frames handed
 * to net_user_input, with the peer address in the IP header. */
static void guest_send(fake_guest *g, u8 flags, u32 ack) {
    u8 ip[20];
    u8 tcp[20];
    memset(ip, 0, sizeof(ip));
    memset(tcp, 0, sizeof(tcp));
    u16 tot = (u16)(sizeof(ip) + sizeof(tcp));
    ip[0] = 0x45;
    put16(ip + 2, tot);
    ip[8] = 64;
    ip[9] = 6;
    put32(ip + 12, GUEST_IP);
    put32(ip + 16, 0x7f000001u); /* 127.0.0.1, where our listener lives */
    put16(tcp, GUEST_PORT);
    put16(tcp + 2, g->rport);
    put32(tcp + 4, 1); /* our seq: the SYN ate 0, everything after is 1 */
    put32(tcp + 8, ack);
    tcp[12] = 0x50;
    tcp[13] = flags;
    put16(tcp + 14, 0xffff);
    put16(tcp + 16, csum16((const u8 *)tcp, 20));
    u8 eth[14];
    memset(eth, 0, sizeof(eth));
    put16(eth + 12, 0x0800);
    u8 frame[54];
    memcpy(frame, eth, 14);
    memcpy(frame + 14, ip, 20);
    memcpy(frame + 34, tcp, 20);
    net_user_input(g->n, frame, sizeof(frame));
}

/* What the emulator's virtio-net would do with a frame for the guest. */
static bool guest_output(void *ud, const u8 *frame, u32 len) {
    fake_guest *g = (fake_guest *)ud;
    g->fills++;
    if (g->fills % REFUSE_EVERY == REFUSE_AT) {
        g->refused++;
        return false; /* no descriptor posted yet: the relay must hold it */
    }
    if (len < 34)
        return true;
    const u8 *ip = frame + 14;
    u32 ihl = (u32)((ip[0] & 0x0f) * 4);
    u32 tot = (u32)((ip[2] << 8) | ip[3]);
    if (ihl < 20 || tot < ihl + 20 || 14u + tot > len)
        return true;
    const u8 *tcp = ip + ihl;
    u32 doff = (u32)((tcp[12] >> 4) * 4);
    u32 dlen = tot - ihl - doff;
    u32 seq = get32(tcp + 4);
    u8 flags = tcp[13];

    if ((flags & 0x12) == 0x12 && dlen == 0) {
        g->gseq = g->first = seq + 1;
        g->started = true;
        return true;
    }
    if (dlen) {
        if (seq != g->gseq)
            return true; /* out of order, and this fake guest does not buffer */
        g->dropped++;
        if ((g->lost_seg++ % LOSE_EVERY) == (LOSE_EVERY - 1))
            return true; /* taken, then lost on the way to the socket */
        /* Store by offset, the way a real stack does: a segment the relay sends
         * twice then lands where it already landed, instead of pushing a byte
         * counter past the end and hiding a genuine gap. */
        u32 off = seq - g->first;
        if (off < TOTAL) {
            if (off + dlen > TOTAL)
                dlen = TOTAL - off;
            memcpy(g_got + off, tcp + doff, dlen);
            if (off + dlen > g->payload)
                g->payload = off + dlen;
        }
        g->gseq = seq + dlen;
    }
    guest_send(g, 0x10, g->gseq);
    return true;
}

void test_net_user_bulk(void) {
    test_begin("net_user_bulk");
    for (u32 i = 0; i < TOTAL; i++)
        g_pattern[i] = (u8)(i * 31u + 7u);

    int ls = socket(AF_INET, SOCK_STREAM, 0);
    CHECK(ls >= 0);
    int on = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0;
    CHECK(bind(ls, (struct sockaddr *)&sa, sizeof(sa)) == 0);
    socklen_t sl = sizeof(sa);
    CHECK(getsockname(ls, (struct sockaddr *)&sa, &sl) == 0);
    CHECK(listen(ls, 8) == 0);
    fcntl(ls, F_SETFL, fcntl(ls, F_GETFL, 0) | O_NONBLOCK);

    fake_guest g;
    memset(&g, 0, sizeof(g));
    g.acc = -1;
    g.rport = ntohs(sa.sin_port);
    CHECK(net_user_init(&g.n, guest_output, &g) == RVM_OK);

    /* SYN, so the relay opens the upstream socket itself. */
    guest_send(&g, 0x02, 0);
    for (int i = 0; i < 50 && g.acc < 0; i++) {
        int a = accept(ls, NULL, NULL);
        if (a >= 0) {
            fcntl(a, F_SETFL, fcntl(a, F_GETFL, 0) | O_NONBLOCK);
            g.acc = a;
        } else
            usleep(2000);
    }
    CHECK(g.acc >= 0);
    CHECK(g.started);

    u32 sent = 0;
    u32 prev = 0;
    double t_start = now_s(), t_prog = t_start;
    while (g.payload < TOTAL && now_s() - t_start < 25.0) {
        while (sent < TOTAL) {
            ssize_t w = send(g.acc, g_pattern + sent, TOTAL - sent, MSG_DONTWAIT);
            if (w > 0) {
                sent += (u32)w;
                continue;
            }
            break; /* the socket buffer is full; the relay will drain it */
        }
        net_user_poll(g.n);
        if (g.payload != prev) {
            prev = g.payload;
            t_prog = now_s();
            continue;
        }
        /* Nothing new.  The relay may be waiting for an ack, or for the device
         * to take a frame -- both resolve on the next poll.  What it cannot do
         * without real time is notice that the guest has been silent long
         * enough to replay, so once progress has truly stalled, nap in steps
         * the retransmission clock can expire during. */
        if (now_s() - t_prog > 0.02)
            usleep(20000);
    }

    CHECK_U64(g.payload, TOTAL);
    CHECK(memcmp(g_got, g_pattern, TOTAL) == 0);
    /* These two are the point of the exercise: had every frame been accepted
     * and every segment arrived, a broken relay could still pass above. */
    CHECK(g.refused > 0);
    CHECK(g.lost_seg / LOSE_EVERY > 0);
    CHECK(g.refused >= 3);

    close(g.acc);
    close(ls);
    net_user_free(g.n);
    test_end();
}
