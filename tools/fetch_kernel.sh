#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
#
# fetch_kernel.sh -- download a Debian riscv64 kernel .deb and extract the
#                    uncompressed ELF vmlinux that RVM boots.
#
#     ./tools/fetch_kernel.sh                       # newest in trixie
#     ./tools/fetch_kernel.sh --version 6.12.57-1   # an exact one
#     ./tools/fetch_kernel.sh --suite trixie --out vmlinux
#
# RVM's ELF loader wants a real ELF64 with PT_LOAD segments, not a compressed
# Image and not an EFI zboot stub.  Debian's linux-image-*-riscv64 ships
# /boot/vmlinux-<ver> for exactly that reason; this script finds it and, if a
# given package only ships vmlinuz-<ver>, decompresses or unpacks it.
#
set -euo pipefail

SUITE="trixie"
COMPONENT="main"
VERSION=""
OUT="vmlinux"
MIRROR="http://deb.debian.org/debian"
ARCHIVE=""
KEEP_DEB=0

usage() {
    cat <<USAGE
usage: $(basename "$0") [options]

  --suite NAME      Debian suite                (default: $SUITE)
  --component NAME  pool component              (default: $COMPONENT)
  --version VER     package version, e.g. 6.12.57-1
                    (default: newest found in the suite's Packages index)
  --archive FILE    use a local .deb instead of downloading
  --out FILE        where to write the ELF      (default: $OUT)
  --keep            keep the downloaded .deb
  -h, --help        this text

Requires: curl or wget; dpkg-deb or ar+tar.
USAGE
}

while [ $# -gt 0 ]; do
    case "$1" in
        --suite)     SUITE="$2"; shift 2 ;;
        --component) COMPONENT="$2"; shift 2 ;;
        --version)   VERSION="$2"; shift 2 ;;
        --archive)   ARCHIVE="$2"; shift 2 ;;
        --out)       OUT="$2"; shift 2 ;;
        --keep)      KEEP_DEB=1; shift ;;
        -h|--help)   usage; exit 0 ;;
        *) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

log() { printf '\033[1;34m[fetch_kernel]\033[0m %s\n' "$*"; }
die() { printf '\033[1;31m[fetch_kernel] error:\033[0m %s\n' "$*" >&2; exit 1; }

# ------------------------------------------------------------------- fetching
fetch() { # url dest
    local url="$1" dest="$2"
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --retry 3 -o "$dest" "$url"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$dest" "$url"
    else
        die "need curl or wget"
    fi
}

WORK="$(mktemp -d /tmp/rvm-kernel.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

if [ -z "$ARCHIVE" ]; then
    if [ -z "$VERSION" ]; then
        log "reading the Packages index for $SUITE/$COMPONENT/riscv64"
        IDX="$WORK/Packages.gz"
        fetch "$MIRROR/dists/$SUITE/$COMPONENT/binary-riscv64/Packages.gz" "$IDX" \
            || die "cannot read $MIRROR/dists/$SUITE/$COMPONENT/binary-riscv64/Packages.gz"
        gunzip -f "$IDX"
        # Newest linux-image-*-riscv64 by version sort.
        VERSION="$(awk '
            /^Package: linux-image-[0-9].*-riscv64$/ { pkg = substr($2, 9) }
            /^Version: / && pkg != "" { print $2 "\t" pkg; pkg = "" }
        ' "$WORK/Packages" | sort -V | tail -1)"
        [ -n "$VERSION" ] || die "no linux-image-*-riscv64 found in $SUITE"
        PKGVER="${VERSION%%	*}"
        PKGNAME="linux-image-${VERSION##*	}"
        log "newest kernel: $PKGNAME version $PKGVER"
        VERSION="$PKGVER"
        PACKAGE="$PKGNAME"
    else
        PACKAGE="linux-image-${VERSION%-*}-riscv64"
        log "requested version $VERSION -> $PACKAGE"
    fi

    # The pool path is pool/<component>/l/linux/<name>_<ver>_<arch>.deb, but the
    # exact directory depends on the source package name, so resolve it from
    # the Packages index when we have one, else probe the usual locations.
    FILENAME=""
    if [ -f "$WORK/Packages" ]; then
        FILENAME="$(awk -v p="$PACKAGE" -v v="$VERSION" '
            $1 == "Package:" && $2 == p { inp = 1 }
            inp && $1 == "Version:" && $2 == v { inv = 1 }
            inp && inv && $1 == "Filename:" { print $2; exit }
        ' "$WORK/Packages")"
    fi
    if [ -z "$FILENAME" ]; then
        SRC="${PACKAGE%%_*}"
        LETTER="$(printf '%s' "${SRC:0:1}")"
        CANDIDATES=(
            "pool/$COMPONENT/l/linux/${PACKAGE}_${VERSION}_riscv64.deb"
            "pool/$COMPONENT/l/linux-signed-riscv64/${PACKAGE}_${VERSION}_riscv64.deb"
            "pool/$COMPONENT/$LETTER/linux/${PACKAGE}_${VERSION}_riscv64.deb"
        )
        for c in "${CANDIDATES[@]}"; do
            if curl -fsSI -o /dev/null "$MIRROR/$c" 2>/dev/null; then FILENAME="$c"; break; fi
        done
    fi
    [ -n "$FILENAME" ] || die "could not locate the .deb for $PACKAGE $VERSION"

    ARCHIVE="$WORK/kernel.deb"
    log "downloading $MIRROR/$FILENAME"
    fetch "$MIRROR/$FILENAME" "$ARCHIVE" || die "download failed"
fi

[ -f "$ARCHIVE" ] || die "no such archive: $ARCHIVE"
log "archive: $ARCHIVE ($(du -h "$ARCHIVE" | cut -f1))"

# ------------------------------------------------------------------ extracting
EX="$WORK/root"
mkdir -p "$EX"
if command -v dpkg-deb >/dev/null 2>&1; then
    dpkg-deb -x "$ARCHIVE" "$EX" || die "dpkg-deb could not extract the archive"
else
    log "dpkg-deb missing; falling back to ar + tar"
    command -v ar >/dev/null 2>&1 || die "need dpkg-deb or ar"
    ( cd "$WORK" && ar x "$ARCHIVE" )
    for t in "$WORK"/data.tar.*; do
        [ -e "$t" ] || continue
        case "$t" in
            *.tar.xz) tar -C "$EX" -xJf "$t" ;;
            *.tar.zst) tar -C "$EX" --zstd -xf "$t" ;;
            *.tar.gz)  tar -C "$EX" -xzf "$t" ;;
            *.tar)     tar -C "$EX" -xf "$t" ;;
        esac
    done
fi

newest() { # newest <glob-name> : highest version sort under $EX/boot
    find "$EX/boot" -maxdepth 1 -type f -name "$1" 2>/dev/null | sort -V | tail -1
}

CAND="$(newest 'vmlinux-*')"
if [ -n "$CAND" ]; then
    log "found uncompressed ELF kernel: ${CAND#"$EX"}"
    cp -f "$CAND" "$OUT"
elif [ -n "$(newest 'vmlinuz-*')" ]; then
    CAND="$(newest 'vmlinuz-*')"
    log "package ships ${CAND#"$EX"}; trying to turn it into an ELF"
    MAGIC="$(od -An -tx1 -N4 "$CAND" | tr -d ' ')"
    case "$MAGIC" in
        7f454c46) log "it is already an ELF"; cp -f "$CAND" "$OUT" ;;
        1f8b*)    log "gzip-compressed Image"; gunzip -c "$CAND" > "$WORK/Image"
                  die "got a raw Image, not an ELF.  Boot it with: ./rvm --kernel $WORK/Image --blob 0x80200000"
                  ;;
        4d5a*)    log "EFI/zboot PE stub"
                  if command -v python3 >/dev/null 2>&1; then
                      python3 - "$CAND" "$WORK/Image" <<'PY' || die "could not unpack the zboot stub"
import struct, sys, zlib, lzma, bz2
src, dst = sys.argv[1], sys.argv[2]
d = open(src, 'rb').read()
# The Linux EFI zboot header: "MZ", ..., then "Linux\0" magic at 0x38-ish with
# the payload offset/size and the compression algorithm name.
i = d.find(b'LIN\0')
if i < 0:
    i = d.find(b'Linux\0')
if i < 0:
    sys.exit('no zboot header found')
off, size = struct.unpack_from('<II', d, i + 8)
algo = d[i+16:i+48].split(b'\0')[0].decode()
payload = d[off:off+size]
if algo in ('', 'none'):
    out = payload
elif algo == 'gzip':
    out = zlib.decompress(payload, 16 + zlib.MAX_WBITS)
elif algo == 'lzma' or algo == 'xz':
    out = lzma.decompress(payload)
elif algo == 'bzip2':
    out = bz2.decompress(payload)
elif algo == 'lz4':
    sys.exit('lz4 zboot payload: install lz4 and rerun, or use a vmlinux-* package')
else:
    sys.exit('unknown zboot algorithm: %r' % algo)
open(dst, 'wb').write(out)
print('unpacked %s (%s) -> %d bytes' % (algo, src, len(out)))
PY
                      die "extracted a raw Image, not an ELF: $WORK/Image"
                  else
                      die "no python3 available to unpack the zboot stub"
                  fi ;;
        *) die "unrecognised kernel image magic 0x$MAGIC in ${CAND#"$EX"}" ;;
    esac
else
    die "the package contains no /boot/vmlinux-* or /boot/vmlinuz-*"
fi

# ---------------------------------------------------------------- verification
MAGIC="$(od -An -tx1 -N4 "$OUT" | tr -d ' ')"
if [ "$MAGIC" = "7f454c46" ]; then
    log "verified ELF: $OUT ($(du -h "$OUT" | cut -f1))"
    if command -v readelf >/dev/null 2>&1; then
        readelf -h "$OUT" | grep -E 'Class|Machine|Entry point' | sed 's/^/    /' || true
    fi
else
    log "warning: $OUT is not an ELF (magic 0x$MAGIC)"
fi

if [ "$KEEP_DEB" = 1 ] && [ "$ARCHIVE" != "${ARCHIVE#"$WORK"}" ]; then
    cp -f "$ARCHIVE" "./$(basename "$ARCHIVE")"
    log "kept ./$(basename "$ARCHIVE")"
fi
log "boot it with: ./rvm --kernel $OUT --disk disk.img"
