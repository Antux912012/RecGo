#!/usr/bin/env bash
# Build a Debian (.deb) package for RecGo using a Debian/Ubuntu Podman container.

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION="0.3.1"
DEB_NAME="recgo_${VERSION}-1_amd64.deb"
STAGE_DIR="${PROJECT_DIR}/deb-staging"
IMAGE="${RECGO_DEB_CONTAINER:-docker.io/library/debian:sid}"

rm -rf "${STAGE_DIR}" "${PROJECT_DIR}/build-deb" "${PROJECT_DIR}/${DEB_NAME}"
mkdir -p "${STAGE_DIR}/DEBIAN"

podman run --rm --network=host --user 0:0 \
    -v "${PROJECT_DIR}:/workspace:Z" \
    -w /workspace \
    "${IMAGE}" \
    bash -c "apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        gcc meson ninja-build valac pkg-config dpkg-dev \
        libgtk-4-dev libadwaita-1-dev libglib2.0-dev \
        libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev && \
        meson setup build-deb --prefix=/usr && \
        meson compile -C build-deb && \
        DESTDIR=/workspace/deb-staging meson install -C build-deb --no-rebuild"

cat > "${STAGE_DIR}/DEBIAN/control" <<EOF
Package: recgo
Version: ${VERSION}-1
Section: video
Priority: optional
Architecture: amd64
Depends: libgtk-4-1 (>= 4.14), libadwaita-1-0 (>= 1.5), libglib2.0-0, libgstreamer1.0-0 (>= 1.20), libgstreamer-plugins-base1.0-0 (>= 1.20), gstreamer1.0-plugins-base, gstreamer1.0-plugins-good, gstreamer1.0-plugins-bad, gstreamer1.0-pipewire
Maintainer: Antonio De Gaetano <antux912012@users.noreply.github.com>
Homepage: https://github.com/Antux912012/RecGo
Description: GPU-accelerated screen, window, and multi-webcam recorder for GNOME
 RecGo is a native GTK4/Libadwaita screen, window, and multi-webcam recorder
 built with Vala, C, and Meson for GNOME 51.
EOF

dpkg-deb --build --root-owner-group "${STAGE_DIR}" "${PROJECT_DIR}/${DEB_NAME}"
rm -rf "${STAGE_DIR}" "${PROJECT_DIR}/build-deb"
echo "==> Debian package created at: ${PROJECT_DIR}/${DEB_NAME}"
