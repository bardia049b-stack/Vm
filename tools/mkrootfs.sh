#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# mkrootfs.sh -- build a bootable Debian riscv64 root filesystem and pack it
#                into disk.img.
#
# This is step 8 of PLAN.  It needs real privileges and a real network, so
# run it on a Debian/Ubuntu host, not inside a restricted container:
#
#     sudo ./tools/mkrootfs.sh --suite trixie --size 2G --out disk.img
#
# What it does, in order:
#   1. installs the host tools (debootstrap, qemu-user-static, e2fsprogs)
#   2. runs debootstrap --variant=minbase for riscv64 under qemu-user-static
#   3. finishes the second stage inside the chroot
#   4. swaps systemd for sysvinit-core (RVM has no cgroup/namespace support)
#   5. writes inittab, fstab, hostname, hosts, a root password and getty on
#      ttyS0
#   6. creates an ext4 image with mke2fs -d, which needs no loop device
#
# The result boots to "rvm login:" with:
#     ./rvm --kernel vmlinux --disk disk.img
#
set -euo pipefail

SUITE="trixie"
MIRROR="http://deb.debian.org/debian"
SIZE="2G"
OUT="disk.img"
ROOTDIR=""
PASSWORD="rvm"
KEEP=0
EXTRA_PKGS="linux-image-riscv64,openssh-server,iproute2,iputils-ping,curl,wget,less,vim-tiny"

usage() {
    cat <<USAGE
usage: $(basename "$0") [options]

  --suite NAME       Debian suite to bootstrap          (default: $SUITE)
  --mirror URL       Debian mirror                      (default: $MIRROR)
  --size SIZE        ext4 image size, e.g. 2G, 4G       (default: $SIZE)
  --out FILE         output disk image                  (default: $OUT)
  --root DIR         reuse/populate this directory instead of a temp one
  --password PASS    root password                      (default: $PASSWORD)
  --packages LIST    comma-separated extra packages     (default: $EXTRA_PKGS)
  --keep             do not delete the rootfs directory when done
  -h, --help         this text

Requires: debootstrap, qemu-user-static, e2fsprogs, root.
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --suite)    SUITE="$2"; shift 2 ;;
        --mirror)   MIRROR="$2"; shift 2 ;;
        --size)     SIZE="$2"; shift 2 ;;
        --out)      OUT="$2"; shift 2 ;;
        --root)     ROOTDIR="$2"; shift 2 ;;
        --password) PASSWORD="$2"; shift 2 ;;
        --packages) EXTRA_PKGS="$2"; shift 2 ;;
        --keep)     KEEP=1; shift ;;
        -h|--help)  usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

log()  { printf '\033[1;34m[mkrootfs]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[mkrootfs] error:\033[0m %s\n' "$*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "must run as root (debootstrap and chroot need it)"

for tool in debootstrap qemu-riscv64-static mke2fs; do
    command -v "$tool" >/dev/null 2>&1 || die "missing $tool -- apt-get install debootstrap qemu-user-static e2fsprogs"
done

ARCH="$(dpkg --print-architecture 2>/dev/null || echo amd64)"
log "host architecture: $ARCH; target: riscv64 ($SUITE)"
[ "$ARCH" = riscv64 ] || log "using qemu-user-static to run riscv64 binaries in the chroot"

# ---------------------------------------------------------------- rootfs dir
CLEANUP=""
if [ -z "$ROOTDIR" ]; then
    ROOTDIR="$(mktemp -d /tmp/rvm-rootfs.XXXXXX)"
    [ "$KEEP" = 1 ] || CLEANUP="$ROOTDIR"
fi
trap 'if [ -n "$CLEANUP" ]; then log "removing $CLEANUP"; rm -rf "$CLEANUP"; fi' EXIT
mkdir -p "$ROOTDIR"
log "rootfs directory: $ROOTDIR"

# ---------------------------------------------------------------- debootstrap
# --foreign only extracts and runs the first stage on the host; the second
# stage runs inside the chroot where the riscv64 libc is already in place.
if [ -f "$ROOTDIR/debootstrap/debootstrap.log" ] &&
   { [ -d "$ROOTDIR/usr/share/riscv64-linux" ] || [ -x "$ROOTDIR/bin/sh" ]; }; then
    log "first stage already present, skipping --first-stage"
else
    log "debootstrap --arch=riscv64 --variant=minbase --foreign $SUITE"
    debootstrap --arch=riscv64 --variant=minbase --foreign \
        --include="$EXTRA_PKGS" "$SUITE" "$ROOTDIR" "$MIRROR"
fi

# qemu-user-static needs to be visible inside the chroot for stage two.
if [ "$ARCH" != riscv64 ]; then
    mkdir -p "$ROOTDIR/usr/bin"
    cp -f "$(command -v qemu-riscv64-static)" "$ROOTDIR/usr/bin/"
    log "copied qemu-riscv64-static into the chroot"
fi

# --------------------------------------------------------------- second stage
log "running --second-stage inside the chroot"
mount --bind /dev  "$ROOTDIR/dev"  2>/dev/null || true
mount --bind /proc "$ROOTDIR/proc" 2>/dev/null || true
mount --bind /sys  "$ROOTDIR/sys"  2>/dev/null || true
umount_chroot() {
    for m in dev proc sys; do
        if mountpoint -q "$ROOTDIR/$m"; then
            umount -l "$ROOTDIR/$m" || true
        fi
    done
}
trap 'umount_chroot; if [ -n "$CLEANUP" ]; then rm -rf "$CLEANUP"; fi' EXIT

chroot "$ROOTDIR" /debootstrap/debootstrap --second-stage

# RVM implements no cgroups and no PID namespace, so systemd cannot run.
# sysvinit-core gives us a real /sbin/init with gettys from inittab.
log "replacing any systemd with sysvinit-core"
chroot "$ROOTDIR" /bin/sh -c '
    set -e
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get -y --no-install-recommends install sysvinit-core sysvinit-utils
    apt-get -y remove --purge systemd systemd-sysv || true
    apt-get -y autoremove --purge || true
'

# ------------------------------------------------------------------ configure
log "writing inittab, fstab, hostname, hosts"

cat > "$ROOTDIR/etc/inittab" <<'INITTAB'
# /etc/inittab -- sysvinit configuration for RVM.
#
# RVM exposes exactly one serial line, ttyS0, at 0x10000000.  Everything the
# user ever sees goes through it, so that is where the getty and the single
# user session live.

id:3:initdefault:

si::sysinit:/etc/init.d/rcS

# Serial console: one getty, no respawn storm if it dies instantly.
T0:23:respawn:/sbin/getty -L ttyS0 115200 vt100

# Only ask for ctrl-alt-delete handling; RVM has no keyboard device yet.
ca:12345:ctrlaltdel:/sbin/shutdown -t1 -a -r now

1:2345:respawn:/sbin/init
2:23:respawn:/sbin/getty 38400 tty2
3:23:respawn:/sbin/getty 38400 tty3

# What to do at the various runlevels.
l0:0:wait:/etc/init.d/rc 0
l1:1:wait:/etc/init.d/rc 1
l2:2:wait:/etc/init.d/rc 2
l3:3:wait:/etc/init.d/rc 3
l4:4:wait:/etc/init.d/rc 4
l5:5:wait:/etc/init.d/rc 5
l6:6:wait:/etc/init.d/rc 6
INITTAB

cat > "$ROOTDIR/etc/fstab" <<'FSTAB'
# /etc/fstab -- the only real block device RVM presents is virtio-blk.
# No fsck at boot, and that is deliberate (passno 0).  The guest is killed, not
# unmounted -- a phone stops the app mid-write -- so passno 1 makes e2fsck walk
# the whole 2 GiB image on every single boot, and at emulator speed a silent
# e2fsck is indistinguishable from a hang in runlevel S.
/dev/vda        /               ext4    defaults,nofail     0   0
proc            /proc           proc    defaults            0   0
sysfs           /sys            sysfs   defaults            0   0
devpts          /dev/pts        devpts  gid=5,mode=620      0   0
tmpfs           /tmp            tmpfs   defaults            0   0
tmpfs           /run            tmpfs   mode=0755,nosuid,nodev  0  0
FSTAB

echo "rvm" > "$ROOTDIR/etc/hostname"
cat > "$ROOTDIR/etc/hosts" <<'HOSTS'
127.0.0.1   localhost
127.0.1.1   rvm
10.0.2.15   rvm           # what the built-in DHCP hands out; see PLAN step 9
HOSTS

cat > "$ROOTDIR/etc/network/interfaces" <<'IFACES'
# /etc/network/interfaces -- virtio-net is the only NIC; the built-in
# userspace NAT hands out 10.0.2.15 with DHCP, like QEMU's user mode.
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet dhcp
IFACES

log "setting the root password"
chroot "$ROOTDIR" /bin/sh -c "echo 'root:$PASSWORD' | chpasswd"

# Make sure the serial console is a valid login terminal.
if ! grep -q '^ttyS0$' "$ROOTDIR/etc/securetty" 2>/dev/null; then
    echo "ttyS0" >> "$ROOTDIR/etc/securetty" 2>/dev/null || true
fi

# --------------------------------------------------------------- kernel image
# The Debian package installs the kernel into /boot as vmlinuz-<ver>, which is
# a compressed image.  RVM boots an ELF vmlinux, so extract it from the package
# if the config enabled CONFIG_EFI_ZBOOT or if vmlinux is not shipped directly.
KVER="$(chroot "$ROOTDIR" /bin/sh -c 'ls /boot/vmlinuz-* 2>/dev/null | sort -V | tail -1' || true)"
if [ -n "$KVER" ]; then
    log "kernel installed in the rootfs: $KVER"
    case "$KVER" in
        */vmlinux-*) cp -f "$ROOTDIR$KVER" ./vmlinux && log "copied ./vmlinux" ;;
        *) log "note: $KVER is a compressed image."
           log "     fetch_kernel.sh extracts the ELF vmlinux for you:"
           log "       ./tools/fetch_kernel.sh --suite $SUITE --out vmlinux" ;;
    esac
fi

# ------------------------------------------------------------------ disk image
log "creating $OUT ($SIZE, ext4)"
rm -f "$OUT"
# mke2fs -d populates the filesystem from a directory tree without needing a
# loop device or mkfs on a real block device -- ideal in a container.
# The journal stays on, unlike the first version of this tool: ext4 without one
# is not meant to survive being unmounted by SIGKILL, which is exactly what a
# phone does to this process.  -M -1 -i 0 switches off the mount-count and
# age triggers for a forced check, so nothing fscks this image behind our back.
mke2fs -t ext4 -d "$ROOTDIR" -F -L rvmroot -M -1 -i 0 "$OUT" "$SIZE"

umount_chroot
CLEANUP=""
trap - EXIT
[ "$KEEP" = 1 ] || { log "removing $ROOTDIR"; rm -rf "$ROOTDIR"; }

log "done: $OUT"
log "boot it with:"
log "  ./rvm --kernel vmlinux --disk $OUT --dtb dtb/rvm.dtb"
log "login as root with password '$PASSWORD'"
