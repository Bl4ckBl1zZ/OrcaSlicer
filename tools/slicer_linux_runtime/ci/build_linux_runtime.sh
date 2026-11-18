#!/usr/bin/env bash
# Builds everything the Windows build takes from Linux for the Bambu "Linux runtime":
#
#   1. the runtime host (tools/slicer_linux_runtime_host, abi0 and abi1 variants), packaged into
#      tools/slicer_linux_runtime_host/runtime/linux-x86_64
#   2. the WSL2 rootfs tar, tools/slicer_linux_runtime/rootfs/windows-wsl2-rootfs.tar
#
# and collects both, ready to hand to the Windows job, into the output directory:
#
#   linux-host-runtime.tar.gz   the linux-x86_64 folder (a tar keeps the executable bits)
#   windows-wsl2-rootfs.tar
#   runtime-files.sha256        checksums of the files inside linux-x86_64
#   SHA256SUMS                  checksums of the three files above
#
# This is the whole "linux-runtime" job of .github/workflows/build.yml; it runs the same way on
# a GitHub-hosted runner, in a bare root container and on a developer machine (WSL included).
#
# Usage: build_linux_runtime.sh [--output-dir DIR] [--build-dir DIR] [--no-install]
#
#   --output-dir DIR   default: <repo>/build-linux-runtime/out
#   --build-dir DIR    CMake build tree of the host, default: <repo>/build-linux-runtime/host
#   --no-install       never install packages; fail when a tool is missing
#
# Missing build tools are installed with apt-get (directly as root, through sudo otherwise).
# The rootfs comes from build_windows_wsl_rootfs.sh: Docker when a daemon answers, otherwise a
# daemonless export; see that script for SLICER_LINUX_RUNTIME_ROOTFS_METHOD and
# SLICER_LINUX_RUNTIME_WSL_ROOTFS_IMAGE.
set -euo pipefail

SCRIPT_DIR="$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)"
REPO_DIR="$(CDPATH='' cd -- "$SCRIPT_DIR/../../.." && pwd)"

OUTPUT_DIR="$REPO_DIR/build-linux-runtime/out"
BUILD_DIR="$REPO_DIR/build-linux-runtime/host"
INSTALL_PACKAGES=1

while [[ $# -gt 0 ]]; do
    case "$1" in
        --output-dir) [[ $# -ge 2 ]] || { echo "--output-dir needs a value" >&2; exit 2; }; OUTPUT_DIR="$2"; shift 2 ;;
        --build-dir)  [[ $# -ge 2 ]] || { echo "--build-dir needs a value" >&2; exit 2; };  BUILD_DIR="$2"; shift 2 ;;
        --no-install) INSTALL_PACKAGES=0; shift ;;
        -h|--help)    sed -n '2,27p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *)            echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
case "$OUTPUT_DIR" in /*) ;; *) OUTPUT_DIR="$PWD/$OUTPUT_DIR" ;; esac
case "$BUILD_DIR" in /*) ;; *) BUILD_DIR="$PWD/$BUILD_DIR" ;; esac

RUNTIME_DIR="$REPO_DIR/tools/slicer_linux_runtime_host/runtime/linux-x86_64"
ROOTFS_TAR="$REPO_DIR/tools/slicer_linux_runtime/rootfs/windows-wsl2-rootfs.tar"

step() { printf '\n==> %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

[[ "$(uname -s)" == "Linux" && "$(uname -m)" == "x86_64" ]] \
    || die "the Linux runtime is linux-x86_64 only (this is $(uname -s) $(uname -m))"

# ---------------------------------------------------------------------------------------------
# Build tools
# ---------------------------------------------------------------------------------------------
# command -> apt package; the two libraries are checked through a header. Of Boost the host
# needs headers only; nlohmann/json comes from deps_src.
missing_packages() {
    local missing=() entry
    for entry in gcc:build-essential g++:build-essential make:build-essential readelf:binutils \
                 cmake:cmake ninja:ninja-build curl:curl tar:tar gzip:gzip sha256sum:coreutils; do
        command -v "${entry%%:*}" >/dev/null 2>&1 || missing+=("${entry#*:}")
    done
    [[ -f /usr/include/openssl/evp.h || -f /usr/local/include/openssl/evp.h ]] || missing+=(libssl-dev)
    [[ -f /usr/include/boost/filesystem/path.hpp || -f /usr/local/include/boost/filesystem/path.hpp ]] || missing+=(libboost-dev)
    # Certificates for the downloads (crane, the base image).
    [[ -f /etc/ssl/certs/ca-certificates.crt ]] || missing+=(ca-certificates)
    [[ ${#missing[@]} -eq 0 ]] || printf '%s\n' "${missing[@]}" | sort -u
}

install_packages() {
    local packages=("$@") run_as_root=()
    command -v apt-get >/dev/null 2>&1 \
        || die "missing packages (${packages[*]}) and no apt-get here; install the equivalents and pass --no-install"
    if [[ "$(id -u)" -ne 0 ]]; then
        if ! command -v sudo >/dev/null 2>&1 || ! sudo -n true 2>/dev/null; then
            die "missing packages (${packages[*]}): not root and no passwordless sudo"
        fi
        run_as_root=(sudo)
    fi
    "${run_as_root[@]}" env DEBIAN_FRONTEND=noninteractive apt-get update -qq
    "${run_as_root[@]}" env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends "${packages[@]}"
}

step "Build tools"
mapfile -t packages < <(missing_packages)
if [[ ${#packages[@]} -gt 0 ]]; then
    [[ "$INSTALL_PACKAGES" == 1 ]] || die "missing packages: ${packages[*]} (--no-install given)"
    echo "installing: ${packages[*]}"
    install_packages "${packages[@]}"
    mapfile -t packages < <(missing_packages)
    [[ ${#packages[@]} -eq 0 ]] || die "still missing after installing: ${packages[*]}"
fi
gcc --version | head -n 1
echo "$(cmake --version | head -n 1), ninja $(ninja --version)"

# ---------------------------------------------------------------------------------------------
# Host binaries
# ---------------------------------------------------------------------------------------------
step "Runtime host (abi1, abi0)"
cmake -S "$REPO_DIR/tools/slicer_linux_runtime_host" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --target slicer_linux_runtime_host

step "Package the host"
bash "$REPO_DIR/tools/slicer_linux_runtime_host/package_linux_host_runtime.sh" "$BUILD_DIR"

for name in slicer_linux_runtime_host slicer_linux_runtime_host_abi1 slicer_linux_runtime_host_abi0; do
    [[ -s "$RUNTIME_DIR/$name" && -x "$RUNTIME_DIR/$name" ]] || die "packaged runtime lacks $name"
done
! grep -q $'\r' "$RUNTIME_DIR/slicer_linux_runtime_host" || die "the host dispatcher script has CR line endings"
# Bambu's libraries are downloaded by the application, never shipped by a build.
for name in libbambu_networking.so libBambuSource.so liblive555.so libagora_rtc_sdk.so libagora-fdkaac.so; do
    [[ ! -e "$RUNTIME_DIR/$name" ]] || die "$name must not be part of the packaged runtime"
done

# Both variants have to start with nothing but the packaged libraries beside them.
for abi in abi1 abi0; do
    answer="$(printf 'x' | LD_LIBRARY_PATH="$RUNTIME_DIR" "$RUNTIME_DIR/slicer_linux_runtime_host_$abi" --probe-stdio-roundtrip)" \
        || die "slicer_linux_runtime_host_$abi does not start"
    [[ "$answer" == "SLICER_RUNTIME_STDIO_OK" ]] || die "slicer_linux_runtime_host_$abi: unexpected probe answer '$answer'"
    echo "slicer_linux_runtime_host_$abi: stdio probe OK"
done

# ---------------------------------------------------------------------------------------------
# WSL rootfs
# ---------------------------------------------------------------------------------------------
step "WSL rootfs"
bash "$REPO_DIR/tools/slicer_linux_runtime/rootfs/build_windows_wsl_rootfs.sh" "$ROOTFS_TAR"

# The host runs inside that rootfs, so nothing packaged may need a newer glibc than it has.
# (A build machine with an older glibc is fine; a newer one produces binaries that do not load.)
step "glibc compatibility of the packaged runtime with the rootfs"
glibc_versions() { grep -ao 'GLIBC_[0-9]\+\.[0-9]\+\(\.[0-9]\+\)\?' "$1" | sort -uV; }
scratch="$(mktemp -d)"
trap 'rm -rf "$scratch"' EXIT
libc_member="$(tar -tf "$ROOTFS_TAR" | grep -E '(^|/)lib/x86_64-linux-gnu/libc\.so\.6$' | head -n 1)"
[[ -n "$libc_member" ]] || die "no x86_64 libc.so.6 in $ROOTFS_TAR"
tar -xf "$ROOTFS_TAR" -C "$scratch" "$libc_member"
rootfs_glibc="$(glibc_versions "$scratch/$libc_member" | tail -n 1)"
[[ -n "$rootfs_glibc" ]] || die "could not read the glibc version of the rootfs"
echo "rootfs provides up to $rootfs_glibc"
for file in "$RUNTIME_DIR"/*; do
    # ELF files only; the dispatcher is a shell script.
    [[ "$(head -c 4 "$file" | od -An -c | tr -d ' ')" == '177ELF' ]] || continue
    needed="$(readelf -V "$file" 2>/dev/null | grep -o 'GLIBC_[0-9]\+\.[0-9]\+\(\.[0-9]\+\)\?' | sort -uV | tail -n 1)"
    echo "$(basename "$file") needs up to ${needed:-no versioned glibc symbol}"
    if [[ -n "$needed" && "$(printf '%s\n%s\n' "$needed" "$rootfs_glibc" | sort -V | tail -n 1)" != "$rootfs_glibc" ]]; then
        die "$(basename "$file") needs $needed, the rootfs has $rootfs_glibc: build on a system whose glibc is not newer than the rootfs image's"
    fi
done

# ---------------------------------------------------------------------------------------------
# Hand-over to the Windows build
# ---------------------------------------------------------------------------------------------
step "Collect"
mkdir -p "$OUTPUT_DIR"
rm -f "$OUTPUT_DIR/linux-host-runtime.tar.gz" "$OUTPUT_DIR/windows-wsl2-rootfs.tar" \
      "$OUTPUT_DIR/runtime-files.sha256" "$OUTPUT_DIR/SHA256SUMS"
tar -C "$(dirname "$RUNTIME_DIR")" --sort=name --owner=0 --group=0 --numeric-owner \
    -czf "$OUTPUT_DIR/linux-host-runtime.tar.gz" "$(basename "$RUNTIME_DIR")"
(cd "$RUNTIME_DIR" && sha256sum -- * ) > "$OUTPUT_DIR/runtime-files.sha256"
if [[ "$(readlink -f "$ROOTFS_TAR")" != "$(readlink -f "$OUTPUT_DIR/windows-wsl2-rootfs.tar")" ]]; then
    cp -f "$ROOTFS_TAR" "$OUTPUT_DIR/windows-wsl2-rootfs.tar"
fi
(cd "$OUTPUT_DIR" && sha256sum linux-host-runtime.tar.gz windows-wsl2-rootfs.tar runtime-files.sha256 > SHA256SUMS)

echo "Linux runtime ready in $OUTPUT_DIR:"
ls -l "$OUTPUT_DIR"
echo
cat "$OUTPUT_DIR/runtime-files.sha256"
