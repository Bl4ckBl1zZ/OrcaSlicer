#!/usr/bin/env bash
# Collects the built Linux runtime host into tools/slicer_linux_runtime_host/runtime/linux-x86_64,
# the folder the Windows build installs as part of its linux_runtime package:
#
#   slicer_linux_runtime_host        dispatcher script (picks the libstdc++ ABI variant)
#   slicer_linux_runtime_host_abi1   host built with _GLIBCXX_USE_CXX11_ABI=1
#   slicer_linux_runtime_host_abi0   host built with _GLIBCXX_USE_CXX11_ABI=0
#   lib*.so*                         non-glibc shared libraries the host needs (libcrypto)
#
# Usage: package_linux_host_runtime.sh [build-dir]      (default: <repo>/build)
#
# Bambu's proprietary libraries are never packaged; the application downloads them.
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PROJECT_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="${1:-$PROJECT_DIR/build}"
case "$BUILD_DIR" in
    /*) ;;
    *) BUILD_DIR="$PWD/$BUILD_DIR" ;;
esac
RUNTIME_ROOT="$PROJECT_DIR/tools/slicer_linux_runtime_host/runtime/linux-x86_64"

if [[ "$(uname -m)" != "x86_64" ]]; then
    echo "this packaging script produces the linux-x86_64 runtime only" >&2
    exit 1
fi

find_host_bin() {
    local name="$1"
    local candidate=""
    for candidate in \
        "$BUILD_DIR/tools/slicer_linux_runtime_host/$name" \
        "$BUILD_DIR/$name"
    do
        if [[ -f "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    find "$BUILD_DIR" -type f -name "$name" 2>/dev/null | head -n 1
}

# Shared libraries the host links against, minus glibc: the runtime rootfs brings its own glibc,
# and mixing in another one breaks the dynamic loader.
collect_runtime_libs() {
    ldd "$1" | awk '
        /=>/ && $3 ~ /^\// { print $3 }
        /^\// { print $1 }
    ' | sort -u
}

HOST_ABI1="$(find_host_bin slicer_linux_runtime_host_abi1 || true)"
HOST_ABI0="$(find_host_bin slicer_linux_runtime_host_abi0 || true)"
if [[ -z "$HOST_ABI1" || ! -f "$HOST_ABI1" || -z "$HOST_ABI0" || ! -f "$HOST_ABI0" ]]; then
    echo "slicer_linux_runtime_host_abi1/abi0 not found under $BUILD_DIR" >&2
    echo "build them first:" >&2
    echo "  cmake -S tools/slicer_linux_runtime_host -B build -G Ninja -DCMAKE_BUILD_TYPE=Release" >&2
    echo "  cmake --build build --target slicer_linux_runtime_host" >&2
    exit 1
fi

rm -rf "$RUNTIME_ROOT"
mkdir -p "$RUNTIME_ROOT"

cp -f "$PROJECT_DIR/tools/slicer_linux_runtime/wsl/slicer_linux_runtime_host" "$RUNTIME_ROOT/slicer_linux_runtime_host"
sed -i 's/\r$//' "$RUNTIME_ROOT/slicer_linux_runtime_host"
cp -f "$HOST_ABI1" "$RUNTIME_ROOT/slicer_linux_runtime_host_abi1"
cp -f "$HOST_ABI0" "$RUNTIME_ROOT/slicer_linux_runtime_host_abi0"
chmod +x "$RUNTIME_ROOT/slicer_linux_runtime_host" "$RUNTIME_ROOT/slicer_linux_runtime_host_abi1" "$RUNTIME_ROOT/slicer_linux_runtime_host_abi0"

mapfile -t libs < <({
    collect_runtime_libs "$HOST_ABI1"
    collect_runtime_libs "$HOST_ABI0"
} | sort -u)
for lib in "${libs[@]}"; do
    base="$(basename -- "$lib")"
    case "$base" in
        ld-linux*|ld-musl-*.so*|linux-vdso.so.*|libc.so.*|libm.so.*|libpthread.so.*|librt.so.*|libdl.so.*|libresolv.so.*|libnss_*.so*|libnsl.so.*|libutil.so.*|libanl.so.*)
            continue
            ;;
    esac
    cp -Lf "$lib" "$RUNTIME_ROOT/"
done

echo "linux host runtime packaged into:"
echo "  $RUNTIME_ROOT"
find "$RUNTIME_ROOT" -maxdepth 1 -type f | sort
