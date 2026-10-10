#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# mkinitrd.sh -- build the small busybox initramfs that rvm boots to a shell.
#
# The full Debian root filesystem (tools/mkrootfs.sh) needs root and a real
# host; this one needs nothing but a network.  It downloads Debian's static
# busybox for riscv64 and packs it into a newc cpio archive that the kernel
# unpacks as an initramfs:
#
#     ./tools/mkinitrd.sh --out initrd.img
#     ./rvm --kernel vmlinux --initrd initrd.img
#
# /init installs the applets, brings eth0 up through the built-in userspace
# NAT and drops into a shell with a controlling tty, so line editing and
# echo behave like a real terminal.  The same archive ships inside the
# Android APK as an asset; copy it over android/app/src/main/assets/ when
# busybox moves on.
#
set -euo pipefail

MIRROR="http://deb.debian.org/debian"
DEB="busybox-static_1.38.0-3+b1_riscv64.deb"
OUT="initrd.img"

usage() {
    cat <<USAGE
usage: $(basename "$0") [options]

  --mirror URL       Debian mirror                       (default: $MIRROR)
  --deb NAME         busybox-static .deb in pool/main/b/busybox
                                                           (default: $DEB)
  --out FILE         output cpio archive                 (default: $OUT)
  -h, --help         this text

Requires: curl, ar, tar (with xz), python3.
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --mirror)   MIRROR="$2"; shift 2 ;;
        --deb)      DEB="$2"; shift 2 ;;
        --out)      OUT="$2"; shift 2 ;;
        -h|--help)  usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

log()  { printf '\033[1;34m[mkinitrd]\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31m[mkinitrd] error:\033[0m %s\n' "$*" >&2; exit 1; }

for tool in curl ar tar python3; do
    command -v "$tool" >/dev/null 2>&1 || die "missing $tool"
done

WORK="$(mktemp -d /tmp/rvm-initrd.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

log "fetching $DEB"
curl -fsSL -o "$WORK/busybox.deb" "$MIRROR/pool/main/b/busybox/$DEB" \
    || die "download failed; pick another --deb from $MIRROR/pool/main/b/busybox/"
( cd "$WORK" && ar x busybox.deb data.tar.xz && tar xf data.tar.xz ./usr/bin/busybox ) \
    || die "not a .deb with usr/bin/busybox inside"

log "laying out the rootfs"
mkdir -p "$WORK"/rootfs/bin "$WORK"/rootfs/proc "$WORK"/rootfs/sys \
         "$WORK"/rootfs/dev "$WORK"/rootfs/tmp "$WORK"/rootfs/etc \
         "$WORK"/rootfs/usr/share/udhcpc
cp "$WORK/usr/bin/busybox" "$WORK/rootfs/bin/busybox"
chmod 755 "$WORK/rootfs/bin/busybox"
ln -sf busybox "$WORK/rootfs/bin/sh"

cat > "$WORK/rootfs/usr/share/udhcpc/default.script" <<'UDHCPC'
#!/bin/sh
case "$1" in
deconfig)
	/bin/ip link set "$interface" up
	;;
bound|renew)
	/bin/ip addr flush dev "$interface" 2>/dev/null
	/bin/ip addr add "$ip/${prefix:-24}" dev "$interface"
	[ -n "$router" ] && /bin/ip route add default via "$router" dev "$interface" 2>/dev/null
	for d in $dns; do echo "nameserver $d"; done > /etc/resolv.conf
	;;
esac
exit 0
UDHCPC
chmod 755 "$WORK/rootfs/usr/share/udhcpc/default.script"

cat > "$WORK/rootfs/init" <<'INIT'
#!/bin/sh
/bin/busybox --install -s /bin
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev 2>/dev/null
# Without a lease script udhcpc never applies the address it obtains.
ifconfig eth0 up 2>/dev/null
# Say so when there is no lease: a silent failure here is what made a dead
# network look like a working one for so long.
if ! udhcpc -i eth0 -n -q -s /usr/share/udhcpc/default.script; then
	echo "=== rvm initramfs: no DHCP lease from 10.0.2.3, network is down ==="
fi

# The initramfs is a debug shell *and* the hand-off into a real rootfs.  A disk
# with /sbin/init takes over as soon as it mounts, so kernel + disk lands in
# Debian instead of stopping here -- which is what "apt: not found" on a phone
# has always meant.  Pass rvm.shell=1 on the kernel command line to stay here.
if ! grep -qs 'rvm.shell=1' /proc/cmdline; then
	mkdir -p /newroot
	if mount /dev/vda /newroot 2>/dev/null || mount /dev/vda1 /newroot 2>/dev/null; then
		if [ -x /newroot/sbin/init ]; then
			echo "=== rvm initramfs: /dev/vda found, handing the console over ==="
			mount --move /dev /newroot/dev 2>/dev/null || mount -t devtmpfs dev /newroot/dev 2>/dev/null
			mount --move /proc /newroot/proc 2>/dev/null || mount -t proc proc /newroot/proc 2>/dev/null
			mount --move /sys /newroot/sys 2>/dev/null || mount -t sysfs sys /newroot/sys 2>/dev/null
			# The lease lives in kernel state, so it walks into the new root
			# with us; /etc/resolv.conf does not, because udhcpc wrote it into
			# the initramfs.  Seed the disk's copy so name resolution works
			# from the first second even on an image whose init scripts are not
			# wired up -- which is what every disk built before the
			# update-rc.d fix in rootfs.yml is.
			RES=$(readlink -f /newroot/etc/resolv.conf 2>/dev/null || echo /newroot/etc/resolv.conf)
			mkdir -p "$(dirname $RES)" 2>/dev/null
			printf 'nameserver 10.0.2.3\noptions timeout:1 attempts:2\n' > $RES 2>/dev/null
			# chroot rather than switch_root: busybox switch_root wants the
			# initramfs to look a particular way and prints its usage when it
			# does not, which leaves a phone in a shell with no reason given.
			# The initramfs stays mounted and costs 1.6 MB nobody misses.
			exec chroot /newroot /sbin/init console=ttyS0
		fi
		umount /newroot 2>/dev/null
	fi
fi

echo
echo "=== rvm initramfs: busybox static ==="
uname -a
exec setsid cttyhack /bin/sh
INIT
chmod 755 "$WORK/rootfs/init"

log "packing $OUT"
python3 - "$WORK/rootfs" "$OUT" <<'PY'
import os, stat, sys

root, out = sys.argv[1], sys.argv[2]

def pad4(n):
    return (4 - n % 4) % 4

def hdr(ino, mode, nlink, filesize, namesize):
    fields = ["070701"]
    for v in (ino, mode, 0, 0, nlink, 0, filesize, 0, 0, 0, 0, namesize, 0):
        fields.append("%08X" % v)
    return "".join(fields).encode()

buf = bytearray()

def emit(name, mode, nlink, data, ino):
    nameb = name.encode() + b"\0"
    buf.extend(hdr(ino, mode, nlink, len(data), len(nameb)))
    buf.extend(nameb)
    buf.extend(b"\0" * pad4(110 + len(nameb)))
    buf.extend(data)
    buf.extend(b"\0" * pad4(len(data)))

ino = 3000000
emit(".", 0o040755, 2, b"", ino)
entries = []
for dirpath, dirnames, filenames in os.walk(root):
    for name in sorted(dirnames) + sorted(filenames):
        entries.append(os.path.join(dirpath, name))
entries.sort(key=lambda p: p.count(os.sep))
for p in entries:
    name = os.path.relpath(p, root)
    st = os.lstat(p)
    ino += 1
    if stat.S_ISDIR(st.st_mode):
        emit(name, 0o040755, 2, b"", ino)
    elif stat.S_ISLNK(st.st_mode):
        emit(name, 0o120777, 1, os.readlink(p).encode(), ino)
    else:
        with open(p, "rb") as fh:
            emit(name, 0o100755, 1, fh.read(), ino)
emit("TRAILER!!!", 0, 1, b"", 0)

with open(out, "wb") as fh:
    fh.write(bytes(buf))
print("%s: %d bytes" % (out, len(buf)))
PY

log "done: $OUT"
log "boot it with:"
log "  ./rvm --kernel vmlinux --initrd $OUT"
