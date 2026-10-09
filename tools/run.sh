#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# run.sh -- the one command that boots RVM with a kernel and a disk.
#
#     ./tools/run.sh                          # vmlinux + disk.img
#     ./tools/run.sh --net                    # also attach the TAP network
#     ./tools/run.sh --gdb                    # stop and wait for a debugger
#     ./tools/run.sh --trace-from 0x80200000  # log retired instructions
#
# Everything after `--` is passed straight through to ./rvm.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

KERNEL="vmlinux"
DISK="disk.img"
DTB=""
INITRD=""
MEM=1024
BOOTARGS="console=ttyS0 earlycon=sbi root=/dev/vda rw rootwait"
NET=0
GDB=0
TAP="rvm0"
TAP_ADDR="10.0.2.1/24"
EXTRA=()

usage() {
    cat <<USAGE
usage: $(basename "$0") [options] [-- extra ./rvm arguments]

  -k, --kernel PATH    kernel image            (default: $KERNEL)
  -d, --disk PATH      virtio-blk image        (default: $DISK)
  -t, --dtb PATH       external DTB            (default: built in-process)
  -i, --initrd PATH    initrd image
  -m, --mem MIB        guest RAM in MiB        (default: $MEM)
  -a, --bootargs STR   kernel command line
      --net            attach virtio-net on the '$TAP' TAP device
                       (needs a build with virtio-net; see PLAN.md step 9)
      --tap NAME       TAP device name         (default: $TAP)
      --trace-from PC  open the instruction trace at that guest PC
      --trace          trace every retired instruction (large!)
      --stats          print instruction counters on exit
      --gdb            run the emulator itself under gdb
      --create-disk    create the disk if it is missing
  -h, --help           this text

Examples
  Build everything and boot:
      ./scripts/build.sh && ./tools/run.sh
  Boot Debian with networking:
      sudo ./tools/run.sh --net
  Debug a jump-to-data bug (trace only from the last known good PC):
      ./tools/run.sh --trace-from 0x80200000
  Debug the emulator itself:
      ./tools/run.sh --gdb
USAGE
}

CREATE_DISK=0
while [ $# -gt 0 ]; do
    case "$1" in
        -k|--kernel)   KERNEL="$2"; shift 2 ;;
        -d|--disk)     DISK="$2"; shift 2 ;;
        -t|--dtb)      DTB="$2"; shift 2 ;;
        -i|--initrd)   INITRD="$2"; shift 2 ;;
        -m|--mem)      MEM="$2"; shift 2 ;;
        -a|--bootargs) BOOTARGS="$2"; shift 2 ;;
        --net)         NET=1; shift ;;
        --tap)         TAP="$2"; shift 2 ;;
        --trace-from)  EXTRA+=(--trace-from "$2"); shift 2 ;;
        --trace)       EXTRA+=(--trace); shift ;;
        --stats)       EXTRA+=(--stats); shift ;;
        --gdb)         GDB=1; shift ;;
        --create-disk) CREATE_DISK=1; shift ;;
        -h|--help)     usage; exit 0 ;;
        --)            shift; EXTRA=("$@"); break ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

log() { printf '\033[1;34m[run]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[run] error:\033[0m %s\n' "$*" >&2; exit 1; }

# ------------------------------------------------------------------ build it
if [ ! -x ./rvm ]; then
    log "no ./rvm binary; building"
    ./scripts/build.sh
fi
[ -x ./rvm ] || die "./rvm did not build"

[ -f "$KERNEL" ] || die "kernel '$KERNEL' not found.
  fetch one with:  ./tools/fetch_kernel.sh --out $KERNEL
  build a rootfs:  sudo ./tools/mkrootfs.sh --out $DISK"

if [ ! -f "$DISK" ]; then
    if [ "$CREATE_DISK" = 1 ]; then
        log "creating an empty $DISK"
        EXTRA+=(--create-disk)
    else
        log "warning: disk '$DISK' is missing; booting without a root device"
        log "         (pass --create-disk, or build one with tools/mkrootfs.sh)"
        DISK=""
    fi
fi

# ------------------------------------------------------------------ networking
# virtio-net is bridged to a TAP device.  The VM does the ARP/DHCP/NAT itself,
# so all the host needs is the interface and IP forwarding.  See PLAN.md step 9.
if [ "$NET" = 1 ]; then
    # Failing here rather than after the TAP device exists: virtio-net is
    # PLAN.md step 9, so most builds of rvm do not know --net at all.
    if ! ./rvm --help 2>&1 | grep -q -- '--net'; then
        die "this build of ./rvm has no --net; virtio-net is PLAN.md step 9"
    fi
    [ "$(id -u)" = 0 ] || die "--net needs root to create the TAP device"
    if ! ip link show "$TAP" >/dev/null 2>&1; then
        log "creating TAP device $TAP"
        ip tuntap add dev "$TAP" mode tap
        ip link set "$TAP" up
        ip addr add "$TAP_ADDR" dev "$TAP"
    else
        log "TAP device $TAP already exists"
    fi
    # Forwarding + masquerade let the guest reach the internet through the host.
    if [ "$(cat /proc/sys/net/ipv4/ip_forward 2>/dev/null || echo 0)" != 1 ]; then
        log "enabling IPv4 forwarding"
        echo 1 > /proc/sys/net/ipv4/ip_forward
    fi
    OUT_IF="$(ip route show default 2>/dev/null | awk '/default/ {print $5; exit}')"
    if [ -n "$OUT_IF" ] && command -v iptables >/dev/null 2>&1; then
        if ! iptables -t nat -C POSTROUTING -s "${TAP_ADDR%/*}.0/24" -o "$OUT_IF" -j MASQUERADE 2>/dev/null; then
            log "adding NAT masquerade via $OUT_IF"
            iptables -t nat -A POSTROUTING -s "${TAP_ADDR%/*}.0/24" -o "$OUT_IF" -j MASQUERADE
        fi
    else
        log "note: no default route or no iptables; guest networking will be link-local only"
    fi
    EXTRA+=(--net --tap "$TAP")
fi

# --------------------------------------------------------------------- launch
ARGS=(-m "$MEM" -k "$KERNEL" -B "$BOOTARGS")
[ -n "$DTB" ]    && ARGS+=(-t "$DTB")
[ -n "$INITRD" ] && ARGS+=(-i "$INITRD")
[ -n "$DISK" ]   && ARGS+=(-d "$DISK")

if [ "$GDB" = 1 ]; then
    command -v gdb >/dev/null || die "--gdb needs gdb installed"
    log "exec: gdb --args ./rvm ${ARGS[*]} ${EXTRA[*]:-}"
    log "     (this debugs the emulator; the guest is traced with --trace-from)"
    exec gdb --args ./rvm "${ARGS[@]}" ${EXTRA[@]+"${EXTRA[@]}"}
fi

log "exec: ./rvm ${ARGS[*]} ${EXTRA[*]:-}"
exec ./rvm "${ARGS[@]}" ${EXTRA[@]+"${EXTRA[@]}"}
