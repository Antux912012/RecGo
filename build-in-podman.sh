#!/usr/bin/env bash
# Build and verify RecGo (Vala + C + Meson) inside the recgo-builder Podman container.

set -euo pipefail

CONTAINER_IMAGE="${RECGO_CONTAINER:-localhost/recgo-builder:latest}"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${RECGO_BUILD_DIR:-build}"

if ! podman image exists "${CONTAINER_IMAGE}" 2>/dev/null; then
    echo "==> Building ${CONTAINER_IMAGE} from Containerfile..."
    podman build -t "${CONTAINER_IMAGE}" -f "${PROJECT_DIR}/Containerfile" "${PROJECT_DIR}"
fi

CMD="${1:-build}"
shift || true

case "${CMD}" in
    shell)
        exec podman run --rm -it --userns=keep-id \
            -v "${PROJECT_DIR}:/workspace:Z" -w /workspace \
            "${CONTAINER_IMAGE}" bash
        ;;
    setup)
        exec podman run --rm --network=none --userns=keep-id \
            -v "${PROJECT_DIR}:/workspace:Z" -w /workspace \
            "${CONTAINER_IMAGE}" \
            meson setup "${BUILD_DIR}" --wipe "$@"
        ;;
    build)
        exec podman run --rm --network=none --userns=keep-id \
            -v "${PROJECT_DIR}:/workspace:Z" -w /workspace \
            "${CONTAINER_IMAGE}" \
            bash -c "[ -d '${BUILD_DIR}' ] || meson setup '${BUILD_DIR}'; meson compile -C '${BUILD_DIR}' $*"
        ;;
    test)
        exec podman run --rm --network=none --userns=keep-id \
            -v "${PROJECT_DIR}:/workspace:Z" -w /workspace \
            "${CONTAINER_IMAGE}" \
            bash -c "[ -d '${BUILD_DIR}' ] || meson setup '${BUILD_DIR}'; meson compile -C '${BUILD_DIR}' && meson test -C '${BUILD_DIR}' --print-errorlogs $*"
        ;;
    clean)
        rm -rf "${PROJECT_DIR}/${BUILD_DIR}"
        ;;
    *)
        exec podman run --rm --network=none --userns=keep-id \
            -v "${PROJECT_DIR}:/workspace:Z" -w /workspace \
            "${CONTAINER_IMAGE}" \
            "${CMD}" "$@"
        ;;
esac
