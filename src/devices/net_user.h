/*
 * net_user.h -- userspace NAT for virtio-net (Android-friendly, no TAP).
 *
 * Guest sees 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3 (QEMU-slirp style).
 * Outbound TCP/UDP go through host sockets; DHCP and DNS are answered here.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RVM_NET_USER_H
#define RVM_NET_USER_H

#include "../rvm.h"

typedef struct net_user net_user;

/* out_frame: deliver a full Ethernet frame to the guest RX path. */
typedef void (*net_user_output_fn)(void *ud, const u8 *frame, u32 len);

rvm_err net_user_init(net_user **out, net_user_output_fn out_fn, void *out_ud);
void net_user_free(net_user *n);

/* Guest transmitted an Ethernet frame (no virtio header). */
void net_user_input(net_user *n, const u8 *frame, u32 len);

/* Service host sockets; may call out_fn with RX frames. */
void net_user_poll(net_user *n);

#endif
