#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# smoke.sh -- boot a guest under a pty and drive it the way a person would.
#
# The unit tests prove the pieces; this proves the product.  It boots the
# busybox initramfs, waits for the shell, types at it one character at a
# time and checks that the guest echoes while typing (a terminal that only
# shows your line after Enter is not a terminal), then checks the network
# the same way: interface, ICMP through the userspace NAT, and a real HTTP
# fetch that needs DNS and TCP.
#
#     ./tools/smoke.sh                      # uses /tmp/vmlinux, /tmp/initrd.img
#     KERNEL=... INITRD=... ./tools/smoke.sh
#
set -euo pipefail

KERNEL="${KERNEL:-/tmp/vmlinux}"
INITRD="${INITRD:-/tmp/initrd.img}"
BUDGET="${BUDGET:-3000000000}"
WITH_NET="${WITH_NET:-0}"

[ -x ./rvm ] || { echo "smoke: build first: make -j2" >&2; exit 2; }
[ -f "$KERNEL" ] || { echo "smoke: no kernel at $KERNEL (tools/fetch_kernel.sh)" >&2; exit 2; }
[ -f "$INITRD" ] || { echo "smoke: no initrd at $INITRD (tools/mkinitrd.sh --out $INITRD)" >&2; exit 2; }

exec python3 -u - "$KERNEL" "$INITRD" <<'PY'
import os, pty, select, sys, tty, time

kernel, initrd = sys.argv[1], sys.argv[2]
budget = os.environ.get("BUDGET", "3000000000")
with_net = os.environ.get("WITH_NET", "0") == "1"

pid, fd = pty.fork()
if pid == 0:
    tty.setraw(sys.stdin.fileno())   # the pty must not echo: only the guest may
    os.execv("./rvm", ["./rvm", "-k", kernel, "-m", "512", "-i", initrd, "-n", budget])

buf = ""
failed = []

def drain(seconds):
    global buf
    end = time.time() + seconds
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.25)
        if r:
            try:
                chunk = os.read(fd, 65536)
            except OSError:
                return
            if not chunk:
                return
            buf += chunk.decode("utf-8", "replace")

def wait_for(needle, seconds):
    end = time.time() + seconds
    while needle not in buf:
        if time.time() > end:
            failed.append("timed out waiting for %r" % needle)
            print("  --- last guest output on timeout ---")
            print("  " + buf[-600:].replace("\n", "\n  "))
            print("  ------------------------------------")
            return False
        drain(0.5)
    return True

def type_line(text, per_char=0.04):
    for ch in text:
        os.write(fd, ch.encode())
        drain(per_char)

def check(name, ok):
    print("  %-46s %s" % (name, "ok" if ok else "FAIL"))
    if not ok:
        failed.append(name)

print("smoke: booting %s with %s" % (kernel, initrd))
check("shell comes up", wait_for("BusyBox", 180))
check("prompt appears", wait_for("# ", 45))
drain(2.0)

cmd = "echo SMOKE_OK"
type_line(cmd)          # no Enter yet
drain(4.0)
if cmd not in buf:
    print("  --- buf tail at echo check ---")
    print("  " + buf[-400:].replace("\n", "\n  "))
    print("  ------------------------------")
check("guest echoes while typing", cmd in buf)
os.write(fd, b"\n")
check("command runs", wait_for("SMOKE_OK\r\n", 20) or buf.count("SMOKE_OK") >= 2)

if with_net:
    os.write(fd, b"cat /proc/net/dev\n")
    check("eth0 exists", wait_for("eth0", 20))
    os.write(fd, b"ping -c 1 -W 3 10.0.2.2\n")
    check("icmp through the nat", wait_for("bytes from", 25))
    os.write(fd, b"wget -T 10 -q -O - http://deb.debian.org/ | head -3\n")
    check("dns + tcp fetch", wait_for("DOCTYPE", 60))
else:
    print("  (network checks skipped: WITH_NET=1 to enable)")

os.write(fd, b"poweroff -f\n")
drain(3.0)
try:
    os.close(fd)
except OSError:
    pass
_, status = os.waitpid(pid, 0)

print("smoke: %s" % ("PASS" if not failed else "FAIL: " + "; ".join(failed)))
sys.exit(0 if not failed else 1)
PY
