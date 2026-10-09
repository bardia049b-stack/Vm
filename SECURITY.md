# Security policy

## Reporting a vulnerability

**Please do not open a public GitHub issue for a security problem.**

Email the address on the maintainer's GitHub profile
([bardia049b-stack](https://github.com/bardia049b-stack)), or use
[GitHub's private vulnerability reporting](https://docs.github.com/en/code-security/security-advisories/guidance-on-reporting-and-writing/privately-reporting-a-security-vulnerability)
if it is enabled on this repository. Subject line: `[RVM security]`.

Include what you found, the shortest guest program or disk image that triggers
it, the version or commit you tested, and what you expected to happen instead.
A proof of concept is worth more than a description; an ASan or UBSan report
with a stack trace is worth more than either.

You should get an acknowledgement within 72 hours. Expect a fix or a documented
mitigation within 14 days for anything that lets a guest reach the host, and a
public advisory once the fix is released. Credit is given in the advisory unless
you ask not to be named.

## Scope

RVM runs untrusted guest code by design — that is the entire point of a virtual
machine — so the boundary that matters is the one between guest and host.

**In scope:**

- Any guest instruction, MMIO access, virtio descriptor or FDT-driven path that
  reads or writes host memory outside the buffers allocated for it.
- Integer overflow, unchecked length or index arithmetic in the device models,
  the loader, the MMU or the virtqueue walker. Guest-controlled values reach all
  of them.
- A descriptor chain that makes the emulator loop forever or allocate without
  bound — guest-triggerable denial of service against the host process.
- Path traversal through anything the guest can influence, including disk
  images, initrds and the DTB.
- Sandbox escape on Android: the guest reaching a file, socket or binder
  service it should not be able to.

**Out of scope:**

- Anything requiring a host process already running with elevated privileges.
  On Android the app runs unprivileged and asks for no permissions at all.
- Bugs in the guest kernel or in Debian itself.
- Side channels that need local access to the host CPU. RVM is a
  software emulator with no constant-time guarantees and does not claim to
  resist cache or timing attacks from another process on the same machine.
- The debug facilities. `--trace`, `RVM_TRACE_FROM` and `MODE=debug` are for
  the person running the emulator, and a release build writes nothing a guest
  controls to a log without going through `rvm_log()`.

## Threat model

The emulator is a single-threaded process that owns one contiguous guest RAM
region. Everything else is a device model reading from and writing to that
region through `bus_load()` / `bus_store()`. The invariants that hold it
together:

- **Every guest-supplied length, offset and index is bounds-checked against the
  region it indexes.** Virtqueue descriptors are the sharpest edge: `addr` and
  `len` come straight from guest memory, so the descriptor walker must reject a
  chain that leaves RAM, that overlaps itself, or that is longer than the queue.
- **Device state is fixed-size and allocated once.** No guest input may cause an
  allocation whose size the guest chose.
- **Host file descriptors are opened by the host, never by the guest.** A disk
  path comes from the command line or from the Android file picker, not from
  anything inside the disk image.
- **No guest data is interpreted as a format string.** `rvm_log()` takes a
  literal format and arguments; the one place a guest-controlled string is
  printed is the console sink, which writes bytes.

`MainActivity` is the only exported component and its only intent filter is the
launcher one. Guest kernels and disk images enter through
`ACTION_OPEN_DOCUMENT`, i.e. the user picks a file in the system picker; the
resulting `content://` URI is copied into the app's private `filesDir` and
treated as untrusted guest data from then on. RVM never dereferences a raw path
from an intent and never grants URI permissions onward.

## Supported versions

| Version | Supported |
|---------|-----------|
| `main`  | Yes — security fixes land here first |
| `0.1.x` | Yes, once `v0.1.0` is tagged |
| Anything older | No |

There is one release line and no long-term-support branches. A security fix is
a patch release; upgrading is always the recommended remediation.

## Hardening that is already in place

- Every CI run builds under AddressSanitizer and UndefinedBehaviorSanitizer with
  `halt_on_error=1` and `detect_leaks=1` (`test.yml`, job `sanitizers`).
- The whole tree compiles with `-Wall -Wextra -Wshadow -Wundef -Wpointer-arith
  -Wcast-qual -Wstrict-prototypes -Wmissing-prototypes`, and CI builds it
  optimised with `-Werror` as well as syntax-checked, because constant folding
  at `-O2` finds warnings that `-fsyntax-only` does not.
- The Android JNI shim is built with `-fvisibility=hidden`, so only the
  `Java_dev_rvm_app_RvmNative_*` entry points are exported from
  `librvm_jni.so`.
- The APK is release-minified and resource-shrunk, and the ProGuard rules keep
  only the members the native side looks up by name.
- No *dangerous* permissions: the manifest declares `INTERNET` and
  `ACCESS_NETWORK_STATE` (neither prompts) plus `FOREGROUND_SERVICE`, which is
  reserved for a future VM service and has no `<service>` behind it yet. There
  is no storage permission — guest images are copied into the app's private
  `filesDir` through the storage access framework.
- No third-party Java dependencies, so there is no transitive supply chain
  inside the APK to audit.
- `android:allowBackup="false"`, so a guest disk image cannot be pulled out
  through `adb backup`.

## If you are shipping this

RVM is a young emulator. Do not use it as the sole isolation boundary for
running code you would not otherwise run, and do not expose its console or its
network device to an untrusted network without a host-level sandbox around the
process. A container, a seccomp filter or Android's own app sandbox are all
reasonable second layers, and none of them require a change to RVM.
