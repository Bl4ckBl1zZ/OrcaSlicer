#!/bin/sh
# Prepares the OrbStack Linux machine that runs Bambu's Linux network plug-in for OrcaSlicer on macOS.
#
#   install_runtime_macos.sh PACKAGE_DIR [PLUGIN_CACHE_DIR]
#
# PACKAGE_DIR is <data_dir>/plugins, holding the runtime package (host binaries, scripts and the x86-64 rootfs).
# The machine is arm64: Rosetta crashes in Bambu's plug-in, so the x86-64 host runs through qemu-user, with the
# rootfs unpacked to /opt/x64root as its sysroot. Ubuntu 26.04 is required for QEMU 10: QEMU 8 (Ubuntu 24.04)
# crashes when the plug-in creates its agent. Every step is skipped when already done, so it is safe to rerun.
set -eu

log() {
    printf '%s\n' "[install_runtime_macos] $*"
}

fail() {
    printf '%s\n' "[install_runtime_macos] error: $*" >&2
    exit 1
}

PACKAGE_DIR="${1:-}"
PLUGIN_CACHE_DIR="${2:-}"
[ -n "$PACKAGE_DIR" ] && [ -d "$PACKAGE_DIR" ] || fail "usage: $0 PACKAGE_DIR [PLUGIN_CACHE_DIR]"

SYSROOT=/opt/x64root
ROOTFS_TAR="$PACKAGE_DIR/windows-wsl2-rootfs.tar"
[ -f "$ROOTFS_TAR" ] || fail "missing $ROOTFS_TAR"

# An application started from Finder does not have the shell's PATH.
ORB="${SLICER_LINUX_RUNTIME_ORB:-}"
if [ -z "$ORB" ]; then
    for candidate in "$(command -v orb 2>/dev/null || true)" /usr/local/bin/orb /opt/homebrew/bin/orb \
                     "$HOME/.orbstack/bin/orb" /Applications/OrbStack.app/Contents/MacOS/bin/orb; do
        if [ -n "$candidate" ] && [ -x "$candidate" ]; then
            ORB="$candidate"
            break
        fi
    done
fi
[ -n "$ORB" ] || fail "OrbStack was not found. Install it from https://orbstack.dev, start it once, then try again."

MACHINE="${SLICER_LINUX_RUNTIME_ORBSTACK_MACHINE:-}"
if [ -z "$MACHINE" ] && [ -f "$PACKAGE_DIR/slicer_linux_runtime_orbstack_machine.txt" ]; then
    MACHINE=$(tr -d ' \t\r\n' < "$PACKAGE_DIR/slicer_linux_runtime_orbstack_machine.txt")
fi
MACHINE="${MACHINE:-orca-bambu-bridge}"

log "package dir: $PACKAGE_DIR"
log "OrbStack: $ORB, machine: $MACHINE"

in_machine() {
    "$ORB" -m "$MACHINE" -u root "$@"
}

if ! "$ORB" list -q 2>/dev/null | grep -qx "$MACHINE"; then
    log "creating OrbStack machine $MACHINE (ubuntu:resolute, arm64)"
    "$ORB" create --arch arm64 ubuntu:resolute "$MACHINE" || fail "could not create OrbStack machine $MACHINE"
fi
in_machine true || fail "OrbStack machine $MACHINE does not start"

if ! in_machine sh -c 'command -v qemu-x86_64' >/dev/null 2>&1; then
    log "installing qemu-user"
    in_machine env DEBIAN_FRONTEND=noninteractive sh -c \
        'apt-get update -q && apt-get install -y -q --no-install-recommends qemu-user' \
        || fail "could not install qemu-user in $MACHINE"
fi

# The machine sees the Mac's file system at the same paths.
ROOTFS_HASH=$(shasum -a 256 "$ROOTFS_TAR" | cut -d ' ' -f1)
if [ "$(in_machine cat "$SYSROOT/.slicer_rootfs_sha256" 2>/dev/null || true)" != "$ROOTFS_HASH" ]; then
    log "unpacking the x86-64 rootfs to $SYSROOT"
    # resolv.conf and hosts are removed so that the emulated host falls back to the machine's own, which
    # qemu-user does for any path that is not in the sysroot.
    in_machine sh -c '
        set -eu
        rm -rf "$1.new"
        mkdir -p "$1.new"
        tar -xf "$2" -C "$1.new"
        rm -f "$1.new/etc/resolv.conf" "$1.new/etc/hosts"
        printf "%s\n" "$3" > "$1.new/.slicer_rootfs_sha256"
        rm -rf "$1"
        mv "$1.new" "$1"' sh "$SYSROOT" "$ROOTFS_TAR" "$ROOTFS_HASH" \
        || fail "could not unpack $ROOTFS_TAR in $MACHINE"
fi

# Exit status 3: the runtime is ready but Bambu's plug-in has not been downloaded yet.
set +e
in_machine env SLICER_LINUX_RUNTIME_EXEC_PREFIX="qemu-x86_64 -L $SYSROOT" \
    sh "$PACKAGE_DIR/slicer_linux_runtime_wsl_run_host.sh" --probe "$PACKAGE_DIR" "$PLUGIN_CACHE_DIR"
probe_status=$?
set -e
case "$probe_status" in
    0) log "The Linux runtime is ready." ;;
    3) log "The Linux runtime is ready; Bambu's plug-in is not downloaded yet." ;;
    *) fail "runtime probe failed with exit status $probe_status" ;;
esac
