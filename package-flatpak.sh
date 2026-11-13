#!/usr/bin/env bash
# Build RecGo inside the Mithrandir Podman container and package it as a Flatpak bundle (.flatpak).

set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${PROJECT_DIR}"

echo "==> Step 1: Compiling RecGo inside Mithrandir Podman container..."
./build-in-podman.sh build

echo "==> Step 2: Staging Flatpak build directory..."
BUILD_DIR="${PROJECT_DIR}/flatpak-staging"
REPO_DIR="${PROJECT_DIR}/flatpak-repo"
BUNDLE_FILE="${PROJECT_DIR}/RecGo.flatpak"

rm -rf "${BUILD_DIR}" "${REPO_DIR}" "${BUNDLE_FILE}"
mkdir -p "${BUILD_DIR}/files/bin" \
         "${BUILD_DIR}/files/share/applications" \
         "${BUILD_DIR}/files/share/icons/hicolor/scalable/apps" \
         "${BUILD_DIR}/files/share/icons/hicolor/512x512/apps" \
         "${BUILD_DIR}/files/share/icons/hicolor/256x256/apps" \
         "${BUILD_DIR}/files/share/icons/hicolor/128x128/apps" \
         "${BUILD_DIR}/files/share/icons/hicolor/64x64/apps" \
         "${BUILD_DIR}/files/share/icons/hicolor/48x48/apps" \
         "${BUILD_DIR}/files/share/icons/hicolor/32x32/apps" \
         "${BUILD_DIR}/files/share/glib-2.0/schemas" \
         "${BUILD_DIR}/files/share/metainfo" \
         "${BUILD_DIR}/export/share/applications" \
         "${BUILD_DIR}/export/share/metainfo" \
         "${BUILD_DIR}/export/share/icons/hicolor/scalable/apps" \
         "${BUILD_DIR}/export/share/icons/hicolor/512x512/apps" \
         "${BUILD_DIR}/export/share/icons/hicolor/256x256/apps" \
         "${BUILD_DIR}/export/share/icons/hicolor/128x128/apps" \
         "${BUILD_DIR}/export/share/icons/hicolor/64x64/apps" \
         "${BUILD_DIR}/export/share/icons/hicolor/48x48/apps" \
         "${BUILD_DIR}/export/share/icons/hicolor/32x32/apps"

cp -f "${PROJECT_DIR}/flatpak-metadata.ini" "${BUILD_DIR}/metadata"

if [[ -f "${PROJECT_DIR}/target/release/recgo" ]]; then
    echo "==> Using optimized release binary..."
    install -Dm755 "${PROJECT_DIR}/target/release/recgo" "${BUILD_DIR}/files/bin/recgo"
else
    echo "==> Using debug binary..."
    install -Dm755 "${PROJECT_DIR}/target/debug/recgo" "${BUILD_DIR}/files/bin/recgo"
fi

install -Dm644 "${PROJECT_DIR}/data/io.github.antonio.RecGo.desktop.in" \
    "${BUILD_DIR}/files/share/applications/io.github.antonio.RecGo.desktop"
install -Dm644 "${PROJECT_DIR}/data/io.github.antonio.RecGo.desktop.in" \
    "${BUILD_DIR}/export/share/applications/io.github.antonio.RecGo.desktop"

# Install new PNG icons at all standard resolutions
for size in 512 256 128 64 48 32; do
    install -Dm644 "${PROJECT_DIR}/data/icons/io.github.antonio.RecGo_${size}x${size}.png" \
        "${BUILD_DIR}/files/share/icons/hicolor/${size}x${size}/apps/io.github.antonio.RecGo.png"
    install -Dm644 "${PROJECT_DIR}/data/icons/io.github.antonio.RecGo_${size}x${size}.png" \
        "${BUILD_DIR}/export/share/icons/hicolor/${size}x${size}/apps/io.github.antonio.RecGo.png"
done

install -Dm644 "${PROJECT_DIR}/data/icons/io.github.antonio.RecGo.svg" \
    "${BUILD_DIR}/files/share/icons/hicolor/scalable/apps/io.github.antonio.RecGo.svg"
install -Dm644 "${PROJECT_DIR}/data/icons/io.github.antonio.RecGo.svg" \
    "${BUILD_DIR}/export/share/icons/hicolor/scalable/apps/io.github.antonio.RecGo.svg"
install -Dm644 "${PROJECT_DIR}/data/io.github.antonio.RecGo.gschema.xml" \
    "${BUILD_DIR}/files/share/glib-2.0/schemas/io.github.antonio.RecGo.gschema.xml"
install -Dm644 "${PROJECT_DIR}/data/io.github.antonio.RecGo.metainfo.xml" \
    "${BUILD_DIR}/files/share/metainfo/io.github.antonio.RecGo.metainfo.xml"
install -Dm644 "${PROJECT_DIR}/data/io.github.antonio.RecGo.metainfo.xml" \
    "${BUILD_DIR}/export/share/metainfo/io.github.antonio.RecGo.metainfo.xml"

if command -v glib-compile-schemas >/dev/null 2>&1; then
    glib-compile-schemas "${BUILD_DIR}/files/share/glib-2.0/schemas"
fi

echo "==> Step 3: Exporting OSTree repo and building RecGo.flatpak bundle..."
podman run --rm \
    --userns=keep-id \
    -v "${PROJECT_DIR}:/workspace:Z" \
    -w /workspace \
    localhost/dev-fedora:latest \
    glib-compile-schemas /workspace/flatpak-staging/files/share/glib-2.0/schemas

flatpak build-export "${REPO_DIR}" "${BUILD_DIR}" stable
flatpak build-bundle "${REPO_DIR}" "${BUNDLE_FILE}" io.github.antonio.RecGo stable

echo "==> Flatpak bundle created at: ${BUNDLE_FILE}"
