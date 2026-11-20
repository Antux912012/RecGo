#!/usr/bin/env bash
# Build an RPM package for RecGo inside the recgo-builder Podman container.

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION="0.3.1"
CONTAINER_IMAGE="${RECGO_CONTAINER:-localhost/recgo-builder:latest}"

if ! podman image exists "${CONTAINER_IMAGE}" 2>/dev/null; then
    podman build -t "${CONTAINER_IMAGE}" -f "${PROJECT_DIR}/Containerfile" "${PROJECT_DIR}"
fi

podman run --rm --network=none --userns=keep-id \
    -v "${PROJECT_DIR}:/workspace:Z" \
    -w /workspace \
    "${CONTAINER_IMAGE}" \
    bash -c "mkdir -p /tmp/rpmbuild/{SOURCES,SPECS,BUILD,RPMS,SRPMS} && \
        tar --exclude='.git' --exclude='build*' --exclude='flatpak-*' --exclude='deb-*' --exclude='target' --exclude='*.flatpak' --exclude='*.deb' --exclude='*.rpm' --transform 's,^\.,recgo-${VERSION},' -czf /tmp/rpmbuild/SOURCES/recgo-${VERSION}.tar.gz . && \
        rpmbuild --define '_topdir /tmp/rpmbuild' -bb packaging/recgo.spec && \
        cp /tmp/rpmbuild/RPMS/x86_64/recgo-${VERSION}-1.*.x86_64.rpm /workspace/"

ls -lh "${PROJECT_DIR}"/recgo-${VERSION}-1.*.x86_64.rpm
