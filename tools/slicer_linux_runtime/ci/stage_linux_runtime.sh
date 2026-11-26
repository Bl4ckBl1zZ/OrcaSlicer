#!/usr/bin/env bash
# Stages the linux-runtime artifact (build_linux_runtime.sh) where the macOS build picks it up: the host
# runtime in tools/slicer_linux_runtime_host/runtime/linux-x86_64 and the rootfs in tools/slicer_linux_runtime/rootfs.
# The bash counterpart of stage_linux_runtime.ps1.
#
#   stage_linux_runtime.sh ARTIFACT_DIR
set -euo pipefail

ARTIFACT_DIR="${1:?usage: $0 ARTIFACT_DIR}"
REPO_DIR="$(cd "$(dirname "$0")/../../.." && pwd)"

for name in SHA256SUMS runtime-files.sha256 linux-host-runtime.tar.gz windows-wsl2-rootfs.tar; do
    [ -f "$ARTIFACT_DIR/$name" ] || { echo "The Linux runtime artifact lacks $name: $ARTIFACT_DIR" >&2; exit 1; }
done

echo "Linux runtime artifact: $ARTIFACT_DIR"
(cd "$ARTIFACT_DIR" && shasum -a 256 -c SHA256SUMS)

runtime_parent="$REPO_DIR/tools/slicer_linux_runtime_host/runtime"
rm -rf "$runtime_parent/linux-x86_64"
mkdir -p "$runtime_parent"
tar -xzf "$ARTIFACT_DIR/linux-host-runtime.tar.gz" -C "$runtime_parent"
echo "Runtime host: $runtime_parent/linux-x86_64"
(cd "$runtime_parent/linux-x86_64" && shasum -a 256 -c "$ARTIFACT_DIR/runtime-files.sha256")

cp -f "$ARTIFACT_DIR/windows-wsl2-rootfs.tar" "$REPO_DIR/tools/slicer_linux_runtime/rootfs/windows-wsl2-rootfs.tar"
echo "Rootfs: $REPO_DIR/tools/slicer_linux_runtime/rootfs/windows-wsl2-rootfs.tar"
