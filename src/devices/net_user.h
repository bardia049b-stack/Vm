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

/* Deliver a full Ethernet frame into the guest's RX path.

   Return true only when the guest really has it.  A false means "not now" --
   the device had no RX descriptor posted, or the buffer was too small -- and
   the caller keeps the frame and tries again on the next poll.  When this
   returned void, a frame the device refused was simply gone, and the relay had
   nothing left to re-send it from: any transfer longer than a few dozen
   frames stalled for good, which is how apt-get update died. */
typedef bool (*net_user_output_fn)(void *ud, const u8 *frame, u32 len);

rvm_err net_user_init(net_user **out, net_user_output_fn out_fn, void *out_ud);
void net_user_free(net_user *n);

/* Guest transmitted an Ethernet frame (no virtio header). */
void net_user_input(net_user *n, const u8 *frame, u32 len);

/* Service host sockets; may call out_fn with RX frames.  Call it whenever the
   guest might have posted new RX descriptors: a refused frame is retried from
   here, so nothing is lost by the poll running often. */
void net_user_poll(net_user *n);

#endif
