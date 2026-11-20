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
    mkdir -p /workspace/flatpak-staging/files/share/app-info/xmls /workspace/flatpak-staging/files/share/app-info/icons/flatpak/64x64 /workspace/flatpak-staging/files/share/app-info/icons/flatpak/128x128 /workspace/flatpak-staging/export/share/app-info/xmls /workspace/flatpak-staging/export/share/app-info/icons/flatpak && \
    cp -f /workspace/data/icons/io.github.Antux912012.RecGo_64x64.png /workspace/flatpak-staging/files/share/app-info/icons/flatpak/64x64/io.github.Antux912012.RecGo.png && \
    cp -f /workspace/data/icons/io.github.Antux912012.RecGo_128x128.png /workspace/flatpak-staging/files/share/app-info/icons/flatpak/128x128/io.github.Antux912012.RecGo.png"

python3 - "${BUILD_DIR}/files/share/app-info/xmls/io.github.Antux912012.RecGo.xml.gz" "${PROJECT_DIR}/data/io.github.Antux912012.RecGo.metainfo.xml" "io.github.Antux912012.RecGo" << 'PYEOF'
import gzip, sys, re, datetime
xml_gz, metainfo_path, app_id = sys.argv[1], sys.argv[2], sys.argv[3]
with open(metainfo_path, "r", encoding="utf-8") as mf:
    raw = mf.read()
def repl_date(m):
    dt = datetime.datetime.strptime(m.group(1), "%Y-%m-%d").replace(tzinfo=datetime.timezone.utc)
    return f'timestamp="{int(dt.timestamp())}"'
raw = re.sub(r'date="(\d{4}-\d{2}-\d{2})"', repl_date, raw)
icons_xml = (
    f'  <icon type="cached" width="128" height="128">{app_id}.png</icon>\n'
    f'  <icon type="cached" width="64" height="64">{app_id}.png</icon>\n'
)
raw = raw.replace("</summary>", "</summary>\n" + icons_xml, 1)
body = raw.split("<component", 1)[1]
composed = f'<?xml version="1.0" encoding="UTF-8"?>\n<components version="0.8" origin="flatpak">\n  <component{body}\n</components>\n'
with gzip.open(xml_gz, "wt", encoding="utf-8") as f:
    f.write(composed)
PYEOF

cp -r "${BUILD_DIR}/files/share/app-info/"* "${BUILD_DIR}/export/share/app-info/"

flatpak build-export "${REPO_DIR}" "${BUILD_DIR}" stable
flatpak build-bundle "${REPO_DIR}" "${BUNDLE_FILE}" io.github.Antux912012.RecGo stable

echo "==> Flatpak bundle created at: ${BUNDLE_FILE}"
