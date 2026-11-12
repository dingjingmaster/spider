#!/bin/sh
set -eu

ARCH="${1:-$(uname -m)}"
VERSION="${2:-0.1.0}"
PACK_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
PROJECT_DIR="$(CDPATH= cd -- "${PACK_DIR}/.." && pwd)"
BUILD_DIR="${PACK_DIR}/build/spider"
ROOTFS_DIR="${PACK_DIR}/build/rootfs"
UI_SRC_DIR="${PROJECT_DIR}/src/web/ui"
SERVICE_SRC="${PACK_DIR}/src/scripts/spider.service"
SPIDER_BIN="${BUILD_DIR}/spider"
STRIP_TOOL="${STRIP:-strip}"
INSTALL_PREFIX="/opt/spider"
SYSTEMD_DIR="/usr/lib/systemd/system"

if [ ! -x "${SPIDER_BIN}" ]; then
    echo "error: spider binary not found: ${SPIDER_BIN}" >&2
    exit 1
fi

if command -v readelf >/dev/null 2>&1; then
    MACHINE="$(LC_ALL=C readelf -h "${SPIDER_BIN}" | sed -n 's/.*Machine:[[:space:]]*//p')"
    case "${ARCH}" in
        amd64)
            case "${MACHINE}" in
                *X86-64*) ;;
                *)
                    echo "error: binary machine '${MACHINE}' does not match arch '${ARCH}'" >&2
                    exit 1
                    ;;
            esac
            ;;
        arm64)
            case "${MACHINE}" in
                AArch64) ;;
                *)
                    echo "error: binary machine '${MACHINE}' does not match arch '${ARCH}'" >&2
                    exit 1
                    ;;
            esac
            ;;
    esac
fi

if [ ! -d "${UI_SRC_DIR}" ]; then
    echo "error: ui directory not found: ${UI_SRC_DIR}" >&2
    exit 1
fi

if [ ! -f "${SERVICE_SRC}" ]; then
    echo "error: service unit not found: ${SERVICE_SRC}" >&2
    exit 1
fi

rm -rf "${ROOTFS_DIR}"
mkdir -p "${ROOTFS_DIR}${INSTALL_PREFIX}/bin"
mkdir -p "${ROOTFS_DIR}${INSTALL_PREFIX}/share/spider/ui"
mkdir -p "${ROOTFS_DIR}${SYSTEMD_DIR}"
mkdir -p "${ROOTFS_DIR}/usr/bin"

install -m 0755 "${SPIDER_BIN}" "${ROOTFS_DIR}${INSTALL_PREFIX}/bin/spider"
"${STRIP_TOOL}" --strip-unneeded "${ROOTFS_DIR}${INSTALL_PREFIX}/bin/spider"

cp -a "${UI_SRC_DIR}/." "${ROOTFS_DIR}${INSTALL_PREFIX}/share/spider/ui/"
find "${ROOTFS_DIR}${INSTALL_PREFIX}/share/spider/ui" -type d -exec chmod 0755 {} +
find "${ROOTFS_DIR}${INSTALL_PREFIX}/share/spider/ui" -type f -exec chmod 0644 {} +

install -m 0644 "${SERVICE_SRC}" "${ROOTFS_DIR}${SYSTEMD_DIR}/spider.service"
ln -s "${INSTALL_PREFIX}/bin/spider" "${ROOTFS_DIR}/usr/bin/spider"

printf '%s\n' "${VERSION}" > "${PACK_DIR}/build/rootfs.version"
printf '%s\n' "${ARCH}" > "${PACK_DIR}/build/rootfs.arch"

echo "rootfs: ${ROOTFS_DIR}"
