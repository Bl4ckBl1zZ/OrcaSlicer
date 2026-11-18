#!/usr/bin/env bash
# Produces the WSL2 rootfs tar the Windows build ships as linux_runtime/windows-wsl2-rootfs.tar:
# the flattened filesystem of the base image, no package added.
#
# Usage: build_windows_wsl_rootfs.sh [output.tar]     (default: next to this script)
#
# Environment:
#   SLICER_LINUX_RUNTIME_WSL_ROOTFS_IMAGE   base image (default: ubuntu:24.04); a digest reference
#                                           (name@sha256:...) pins it
#   SLICER_LINUX_RUNTIME_ROOTFS_METHOD      auto (default) | docker | crane
#       docker   docker pull + create + export; needs a working Docker daemon
#       crane    daemonless "crane export" of the same image (go-containerregistry), for
#                environments without Docker such as a bare CI container. The pinned release
#                below is downloaded and checksum-verified, unless SLICER_LINUX_RUNTIME_CRANE
#                names an existing crane binary to use instead.
#       auto     docker when a daemon answers, crane otherwise
#   SLICER_LINUX_RUNTIME_TOOLS_CACHE        where the downloaded crane is kept
#                                           (default: ${TMPDIR:-/tmp}/slicer-linux-runtime-tools)
#
# Both methods export the same linux/amd64 image, file for file. "docker export" additionally
# carries what Docker sets up in every container: .dockerenv, /dev/console, /dev/pts, /dev/shm,
# emptied /etc/hostname, /etc/hosts and /etc/resolv.conf, and an /etc/mtab symlink. WSL creates
# the /dev entries, hostname, hosts and resolv.conf itself when the distro starts, so only the
# /etc/mtab symlink is added to a crane export to make the two filesystems alike.
set -euo pipefail

SCRIPT_DIR="$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)"
OUTPUT_TAR="${1:-$SCRIPT_DIR/windows-wsl2-rootfs.tar}"
BASE_IMAGE="${SLICER_LINUX_RUNTIME_WSL_ROOTFS_IMAGE:-ubuntu:24.04}"
METHOD="${SLICER_LINUX_RUNTIME_ROOTFS_METHOD:-auto}"
PLATFORM="linux/amd64"

# go-containerregistry release used by the crane method.
CRANE_VERSION="v0.22.1"
CRANE_ARCHIVE="go-containerregistry_Linux_x86_64.tar.gz"
CRANE_SHA256="0ab7a1d6932a213aed964ce97666c3077fe691c8606413674a8b3e0b9ec4cda0"
CRANE_URL="https://github.com/google/go-containerregistry/releases/download/${CRANE_VERSION}/${CRANE_ARCHIVE}"

log() { printf '%s\n' "$*" >&2; }
die() { log "error: $*"; exit 1; }

docker_usable() {
    command -v docker >/dev/null 2>&1 || return 1
    if command -v timeout >/dev/null 2>&1; then
        timeout 20 docker info >/dev/null 2>&1
    else
        docker info >/dev/null 2>&1
    fi
}

export_with_docker() {
    local container
    container="slicer-linux-runtime-rootfs-$(date +%s)-$$"
    # shellcheck disable=SC2064
    trap "docker rm -f '$container' >/dev/null 2>&1 || true" EXIT

    docker pull --platform "$PLATFORM" "$BASE_IMAGE" >/dev/null
    log "image: $(docker image inspect --format '{{if .RepoDigests}}{{index .RepoDigests 0}}{{else}}{{.Id}}{{end}}' "$BASE_IMAGE" 2>/dev/null || echo "$BASE_IMAGE")"
    docker create --platform "$PLATFORM" --name "$container" "$BASE_IMAGE" /bin/sh -lc 'exit 0' >/dev/null
    docker export "$container" -o "$OUTPUT_TAR"
    docker rm -f "$container" >/dev/null 2>&1 || true
    trap - EXIT
}

# Prints the path of a usable crane binary, downloading the pinned release when needed.
resolve_crane() {
    if [[ -n "${SLICER_LINUX_RUNTIME_CRANE:-}" ]]; then
        [[ -x "$SLICER_LINUX_RUNTIME_CRANE" ]] || die "SLICER_LINUX_RUNTIME_CRANE is not an executable: $SLICER_LINUX_RUNTIME_CRANE"
        printf '%s\n' "$SLICER_LINUX_RUNTIME_CRANE"
        return 0
    fi

    [[ "$(uname -s)-$(uname -m)" == "Linux-x86_64" ]] || die "the pinned crane download is linux-x86_64 only; set SLICER_LINUX_RUNTIME_CRANE"
    local tool
    for tool in curl tar sha256sum; do
        command -v "$tool" >/dev/null 2>&1 || die "$tool is needed to download crane"
    done

    local cache_root="${SLICER_LINUX_RUNTIME_TOOLS_CACHE:-${TMPDIR:-/tmp}/slicer-linux-runtime-tools}"
    local cache_dir="$cache_root/crane-$CRANE_VERSION"
    local crane="$cache_dir/crane"
    local archive="$cache_dir/$CRANE_ARCHIVE"

    mkdir -p "$cache_dir"
    if [[ ! -f "$archive" ]] || ! printf '%s  %s\n' "$CRANE_SHA256" "$archive" | sha256sum -c --status -; then
        log "downloading crane $CRANE_VERSION"
        rm -f "$archive" "$crane"
        curl -fsSL --retry 5 --retry-delay 5 --retry-all-errors -o "$archive.part" "$CRANE_URL"
        mv -f "$archive.part" "$archive"
        printf '%s  %s\n' "$CRANE_SHA256" "$archive" | sha256sum -c --status - \
            || { rm -f "$archive"; die "crane $CRANE_VERSION archive does not match its pinned SHA256"; }
    fi
    # Always unpacked from the verified archive, so a tampered cached binary is never run.
    tar -xzf "$archive" -C "$cache_dir" crane
    chmod +x "$crane"
    printf '%s\n' "$crane"
}

export_with_crane() {
    local crane
    crane="$(resolve_crane)"
    log "crane: $("$crane" version 2>/dev/null || echo unknown) ($crane)"
    # The digest Docker reports for the same tag (the multi-platform index when there is one).
    log "image: ${BASE_IMAGE%%@*}@$("$crane" digest "$BASE_IMAGE")"
    "$crane" export --platform "$PLATFORM" "$BASE_IMAGE" "$OUTPUT_TAR"

    # The symlink Docker gives every container (see the header).
    local members extra
    members="$(tar -tf "$OUTPUT_TAR")"
    if ! grep -Eq '^(\./)?etc/mtab$' <<<"$members"; then
        extra="$(mktemp -d)"
        mkdir -p "$extra/etc"
        ln -s /proc/mounts "$extra/etc/mtab"
        tar --append -f "$OUTPUT_TAR" -C "$extra" --owner=0 --group=0 --numeric-owner etc/mtab
        rm -rf "$extra"
    fi
}

case "$METHOD" in
    auto)
        if docker_usable; then METHOD=docker; else METHOD=crane; fi
        ;;
    docker)
        docker_usable || die "SLICER_LINUX_RUNTIME_ROOTFS_METHOD=docker, but no Docker daemon answers"
        ;;
    crane)
        ;;
    *)
        die "unknown SLICER_LINUX_RUNTIME_ROOTFS_METHOD '$METHOD' (auto, docker or crane)"
        ;;
esac

mkdir -p "$(dirname -- "$OUTPUT_TAR")"
rm -f "$OUTPUT_TAR"

log "exporting $BASE_IMAGE ($PLATFORM) with $METHOD"
"export_with_$METHOD"

# A truncated or empty export still is a tar file; make sure it holds a root filesystem.
[[ -s "$OUTPUT_TAR" ]] || die "failed to create rootfs tar: $OUTPUT_TAR"
chmod 644 "$OUTPUT_TAR"   # "docker export -o" writes it 0600
listing="$(tar -tf "$OUTPUT_TAR")" || die "rootfs tar is not readable: $OUTPUT_TAR"
for wanted in '(^|/)usr/lib/os-release$' '(^|/)libc\.so\.6$' '(^|/)usr/bin/(da)?sh$'; do
    grep -Eq "$wanted" <<<"$listing" || die "rootfs tar holds no entry matching $wanted: $OUTPUT_TAR"
done

echo "WSL rootfs created ($METHOD, $(wc -l <<<"$listing") entries, $(du -h "$OUTPUT_TAR" | cut -f1)):"
echo "  $OUTPUT_TAR"
