#!/usr/bin/env bash
# Build and verify RecGo inside the Mithrandir Podman development container.
# Supports both localhost/dev-fedora:latest (default) and localhost/dev-debian:latest.

set -euo pipefail

CONTAINER_IMAGE="${RECGO_CONTAINER:-localhost/dev-fedora:latest}"
PROJECT_DIR="/home/antonio/Projects/RecGo"
CARGO_ARGS=("${@:-check}")

if [[ "$CONTAINER_IMAGE" == *"fedora"* ]]; then
    PREP_CMD="rpm -q gstreamer1-devel >/dev/null 2>&1 || sudo dnf install -y --setopt=install_weak_deps=False gstreamer1-devel gstreamer1-plugins-base-devel gstreamer1-plugins-bad-free-devel pipewire-devel"
else
    PREP_CMD="dpkg -s libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstreamer-plugins-bad1.0-dev >/dev/null 2>&1 || (sudo apt-get update && sudo apt-get install -y libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev libgstreamer-plugins-bad1.0-dev libpipewire-0.3-dev)"
fi

exec podman run --rm \
    --userns=keep-id \
    -v "${PROJECT_DIR}:/workspace:Z" \
    -w /workspace \
    "${CONTAINER_IMAGE}" \
    bash -c "${PREP_CMD} && cargo ${CARGO_ARGS[*]}"
