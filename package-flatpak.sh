#!/usr/bin/env bash
# Build RecGo (Vala + C + Meson) inside the recgo-builder Podman container and package as a Flatpak bundle.

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${PROJECT_DIR}"

echo "==> Step 1: Compiling RecGo inside recgo-builder Podman container..."
./build-in-podman.sh setup -Dprefix=/app
./build-in-podman.sh build

echo "==> Step 2: Staging Flatpak build directory..."
BUILD_DIR="${PROJECT_DIR}/flatpak-staging"
REPO_DIR="${PROJECT_DIR}/flatpak-repo"
BUNDLE_FILE="${PROJECT_DIR}/RecGo.flatpak"

rm -rf "${BUILD_DIR}" "${REPO_DIR}" "${BUNDLE_FILE}"
mkdir -p "${BUILD_DIR}/files" "${BUILD_DIR}/export"

cp -f "${PROJECT_DIR}/flatpak-metadata.ini" "${BUILD_DIR}/metadata"

./build-in-podman.sh bash -c "DESTDIR=/workspace/flatpak-staging/files meson install -C build --no-rebuild && \
    cp -r /workspace/flatpak-staging/files/app/* /workspace/flatpak-staging/files/ && \
    rm -rf /workspace/flatpak-staging/files/app && \
    mkdir -p /workspace/flatpak-staging/export/share && \
    cp -r /workspace/flatpak-staging/files/share/applications /workspace/flatpak-staging/export/share/ && \
    cp -r /workspace/flatpak-staging/files/share/metainfo /workspace/flatpak-staging/export/share/ && \
    cp -r /workspace/flatpak-staging/files/share/icons /workspace/flatpak-staging/export/share/ && \
    glib-compile-schemas /workspace/flatpak-staging/files/share/glib-2.0/schemas && \
    mkdir -p /workspace/flatpak-staging/files/share/app-info/xmls /workspace/flatpak-staging/files/share/app-info/icons/flatpak /workspace/flatpak-staging/export/share/app-info/xmls /workspace/flatpak-staging/export/share/app-info/icons/flatpak && \
    appstream-compose --prefix=/workspace/flatpak-staging/files --output-dir=/workspace/flatpak-staging/files/share/app-info/xmls --icons-dir=/workspace/flatpak-staging/files/share/app-info/icons/flatpak --origin=flatpak --basename=io.github.Antux912012.RecGo io.github.Antux912012.RecGo && \
    cp -r /workspace/flatpak-staging/files/share/app-info/* /workspace/flatpak-staging/export/share/app-info/"

flatpak build-export "${REPO_DIR}" "${BUILD_DIR}" stable
flatpak build-bundle "${REPO_DIR}" "${BUNDLE_FILE}" io.github.Antux912012.RecGo stable

echo "==> Flatpak bundle created at: ${BUNDLE_FILE}"
