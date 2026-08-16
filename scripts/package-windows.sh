#!/bin/sh
#
# Build and package the Windows release: the two role libraries, the libusb-1.0
# and libusbK drop-in DLLs, the device examples, and the source of the device
# core and the class layer - 32- and 64-bit, in one zip.
#
# The release workflow does nothing but run this, so the archive attached to a
# tag is the archive you get here:
#
#   scripts/package-windows.sh              build, stage, check, zip -> ./dist
#   scripts/package-windows.sh --no-build   stage from the existing build-x86/x64
#   OUT_DIR=/tmp/pkg scripts/package-windows.sh
#
# The archive lands in the directory you run this from, never in the tree.
#
# Needs: both mingw-w64 cross toolchains (i686 and x86_64), python3 - the build
# runs scripts/gen_exports.py - and zip, file, make.

set -eu

usage() {
    cat <<'EOF'
usage: package-windows.sh [--no-build]

  --no-build   skip the cross-compile and stage whatever build-x86 / build-x64
               already hold; for iterating on the packaging itself

  OUT_DIR      where to write the archive (default: ./dist)
EOF
}

build=1
while [ $# -gt 0 ]; do
    case $1 in
        --no-build) build=0 ;;
        -h|--help)  usage; exit 0 ;;
        *)          echo "package-windows.sh: unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

# the tree this script belongs to, wherever it was run from
TREE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

mkdir -p "${OUT_DIR:-dist}"
OUT=$(CDPATH= cd -- "${OUT_DIR:-dist}" && pwd)

cd "$TREE"

VERSION=$(sed -n 's/.*define USBIP_VERSION *"\(.*\)".*/\1/p' include/usbip.h)
NAME=libusbip-$VERSION-windows
ROOT=$OUT/$NAME

ARCHES='x86 x64'
# an architecture's mingw-w64 target triple, and what `file` calls it in a PE
triple()  { case $1 in x86) echo i686-w64-mingw32 ;; x64) echo x86_64-w64-mingw32 ;; esac; }
pe_arch() { case $1 in x86) echo 'Intel 80386'    ;; x64) echo 'x86-64'           ;; esac; }

# The examples the package does NOT ship: the two doc snippets, which exist to
# keep the guide pages compiling rather than to be run, and the host-role
# clients - a device package has no use for the importing side.
SKIP_EXAMPLES='boot_keyboard host_drive cdc_host uvc_host host_probe'

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
# One build directory per architecture: both toolchains name their objects,
# libraries and executables identically. The default goal covers src/, examples/
# and wrapper/ - and under OS=Windows_NT the wrapper directory builds libusbK
# beside libusb-1.0.
if [ "$build" = 1 ]; then
    jobs=$(nproc 2>/dev/null || echo 4)
    for arch in $ARCHES; do
        echo "=== build $arch ==="
        make -j"$jobs" OS=Windows_NT CROSS_COMPILE="$(triple "$arch")-" BUILD_DIR="build-$arch"
    done
fi

# ---------------------------------------------------------------------------
# Stage
# ---------------------------------------------------------------------------
echo "=== stage $NAME ==="
rm -rf "$ROOT"

# Headers: the two role APIs, the class layer's, and the drop-ins'. The class
# headers keep their classes/ directory - that is how the class sources spell
# them - so one -Iinclude serves all of it.
mkdir -p "$ROOT/include/libusb-1.0" "$ROOT/include/libusbK"
cp include/usbip.h include/usbip-device.h include/usbip-host.h "$ROOT/include/"
cp -r include/classes "$ROOT/include/"
cp wrapper/libusb-1.0/include/libusb.h "$ROOT/include/libusb-1.0/"
cp wrapper/libusbK/include/libusbk.h   "$ROOT/include/libusbK/"
cp scripts/README.dist.md "$ROOT/README.md"

# Source: the device core and the class layer, and nothing else - no Makefiles,
# no host role, no wrapper sources. The private headers come along because the
# core includes them by name from its own directory.
mkdir -p "$ROOT/src/classes/device"
cp src/usbip_device.c src/usbip.c src/pcap.c \
   src/usbip_device_internal.h src/pcap.h src/os_compat.h "$ROOT/src/"
cp src/classes/usb_class.c "$ROOT/src/classes/"
cp src/classes/device/*.c  "$ROOT/src/classes/device/"

# x86 and x64 hold the same file names, one set per architecture. The drop-ins
# keep a directory of their own: they are named after the libraries they
# replace, so putting them on a search path shared with anything else would
# hijack every libusb / libusbK program that finds it.
for arch in $ARCHES; do
    mkdir -p "$ROOT/lib/$arch" "$ROOT/dropin/$arch" "$ROOT/bin/$arch"
    cp "src/build-$arch"/libusbip-*       "$ROOT/lib/$arch/"
    cp "wrapper/build-$arch"/libusb-1.0.* "$ROOT/dropin/$arch/"
    cp "wrapper/build-$arch"/libusbK.dll  "$ROOT/dropin/$arch/"
    for exe in "examples/build-$arch"/*.exe; do
        base=$(basename "$exe" .exe)
        case " $SKIP_EXAMPLES " in *" $base "*) continue ;; esac
        cp "$exe" "$ROOT/bin/$arch/"
    done
done

# ---------------------------------------------------------------------------
# Check, before any of it becomes an archive
# ---------------------------------------------------------------------------
# A 64-bit binary under x86 links against nothing and fails at load time with no
# useful message, so check the architecture of every one of them.
echo "=== check ==="
for arch in $ARCHES; do
    want=$(pe_arch "$arch")
    for f in "$ROOT/lib/$arch"/*.dll "$ROOT/dropin/$arch"/*.dll "$ROOT/bin/$arch"/*.exe; do
        file -b "$f" | grep -q "$want" || {
            echo "not $arch ($want): $f" >&2
            exit 1
        }
    done
    echo "OK: $arch binaries are all $want"
done

# The shipped source has no build system, so -Iinclude is all it may need.
# Compiled outside the package, so a stray include of the tree it came from
# fails here rather than for whoever downloads it.
for arch in $ARCHES; do
    rm -rf "$OUT/srccheck"
    mkdir -p "$OUT/srccheck"
    (cd "$OUT/srccheck" && "$(triple "$arch")-gcc" \
        -std=gnu11 -Wall -Wextra -Werror -Wno-scalar-storage-order -O2 -c \
        -I"$ROOT/include" \
        "$ROOT"/src/*.c "$ROOT"/src/classes/*.c "$ROOT"/src/classes/device/*.c)
    echo "OK: shipped source compiles for $arch ($(ls "$OUT/srccheck"/*.o | wc -l) objects)"
done
rm -rf "$OUT/srccheck"

# ---------------------------------------------------------------------------
# Archive
# ---------------------------------------------------------------------------
echo "=== archive ==="
rm -f "$OUT/$NAME.zip"
(cd "$OUT" && zip -qry "$NAME.zip" "$NAME")

find "$ROOT" -type f | sed "s|^$OUT/||" | sort
echo
echo "staged:  $ROOT  ($(du -sh "$ROOT" | cut -f1))"
echo "archive: $OUT/$NAME.zip  ($(du -h "$OUT/$NAME.zip" | cut -f1))"
