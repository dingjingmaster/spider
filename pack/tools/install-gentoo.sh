#!/bin/sh
set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
PACK_DIR="${SPIDER_PACK_DIR:-$(CDPATH= cd -- "${SCRIPT_DIR}/.." && pwd)}"
DIST_DIR="${PACK_DIR}/dist"
TMP_ROOT="${TMPDIR:-/tmp}"
WORK_DIR=""
CATEGORY="app-admin"
PN="spider-crawler"
ATOM="${CATEGORY}/${PN}"

usage()
{
    echo "Usage: $0 [spider-crawler-gentoo-overlay-<arch>-<version>.tar.gz]" >&2
}

cleanup()
{
    if [ -n "${WORK_DIR:-}" ] && [ -d "${WORK_DIR}" ]; then
        rm -rf "${WORK_DIR}"
    fi
}
trap cleanup EXIT INT TERM

normalize_arch()
{
    case "$1" in
        x86_64)
            echo "amd64"
            ;;
        aarch64|arm64)
            echo "arm64"
            ;;
        *)
            echo "$1"
            ;;
    esac
}

uname_m()
{
    if [ -n "${SPIDER_TEST_UNAME_M:-}" ]; then
        echo "${SPIDER_TEST_UNAME_M}"
    else
        uname -m
    fi
}

find_default_package()
{
    arch="$(normalize_arch "$(uname_m)")"

    find "${DIST_DIR}" -maxdepth 1 -type f \
        -name "spider-crawler-gentoo-overlay-${arch}-*.tar.gz" -print \
        | sort -V | tail -n 1
}

require_root()
{
    if [ "$(id -u)" -ne 0 ]; then
        echo "error: emerge install requires root. Run with sudo: sudo $0${1:+ \"$1\"}" >&2
        exit 1
    fi
}

find_overlay_dir()
{
    root="$1"

    find "${root}" -mindepth 1 -maxdepth 4 -type f -path '*/profiles/repo_name' -print \
        | while IFS= read -r repo_name; do
            dirname "$(dirname "${repo_name}")"
            break
        done
}

detect_accept_keywords()
{
    arch="$(uname_m)"

    case "${arch}" in
        x86_64)
            echo "~amd64"
            ;;
        aarch64|arm64)
            echo "~arm64"
            ;;
        *)
            echo "~${arch}"
            ;;
    esac
}

prepare_overlay_permissions()
{
    overlay="$1"

    chmod 0755 "${WORK_DIR}"
    find "${overlay}" -type d -exec chmod 0755 {} +
    find "${overlay}" -type f -exec chmod 0644 {} +
    chmod 0755 "${overlay}/${CATEGORY}/${PN}/files/spider"
}

prepare_overlay()
{
    overlay="$1"
    ebuild_path="$(find "${overlay}/${CATEGORY}/${PN}" -maxdepth 1 -type f -name "${PN}-*.ebuild" -print | sort | tail -n 1)"

    if [ -z "${ebuild_path}" ] || [ ! -f "${ebuild_path}" ]; then
        echo "error: ebuild not found in overlay: ${overlay}" >&2
        exit 1
    fi

    prepare_overlay_permissions "${overlay}"

    if command -v ebuild >/dev/null 2>&1; then
        PORTDIR_OVERLAY="${overlay}" ebuild "${ebuild_path}" manifest
    elif command -v egencache >/dev/null 2>&1; then
        egencache --repo="$(cat "${overlay}/profiles/repo_name")" \
            --repositories-configuration="${overlay}" --update
    else
        echo "warning: neither ebuild nor egencache found; trying emerge without pre-generated metadata" >&2
    fi
}

prepare_portage_permissions()
{
    overlay="$1"
    portage_tmp="$2"
    distfiles="$3"
    packages="$4"

    prepare_overlay_permissions "${overlay}"
    chmod 01777 "${portage_tmp}" "${distfiles}" "${packages}"
}

run_emerge()
{
    overlay="$1"
    portage_tmp="${WORK_DIR}/portage-tmp"
    distfiles="${WORK_DIR}/distfiles"
    packages="${WORK_DIR}/packages"

    mkdir -p "${portage_tmp}" "${distfiles}" "${packages}"
    prepare_portage_permissions "${overlay}" "${portage_tmp}" "${distfiles}" "${packages}"

    PORTDIR_OVERLAY="${overlay}" \
    PORTAGE_TMPDIR="${portage_tmp}" \
    DISTDIR="${distfiles}" \
    PKGDIR="${packages}" \
    ACCEPT_KEYWORDS="${ACCEPT_KEYWORDS:-$(detect_accept_keywords)}" \
    ACCEPT_LICENSE="${ACCEPT_LICENSE:-*}" \
    emerge --verbose --oneshot --reinstall-atoms="${ATOM}" "${ATOM}"
}

PACKAGE_PATH="${1:-}"
if [ $# -gt 1 ]; then
    usage
    exit 1
fi

if [ "${PACKAGE_PATH}" = "--print-default-package" ]; then
    find_default_package
    exit 0
fi

if [ -z "${PACKAGE_PATH}" ]; then
    PACKAGE_PATH="$(find_default_package)"
fi

if [ -z "${PACKAGE_PATH}" ] || [ ! -f "${PACKAGE_PATH}" ]; then
    echo "error: gentoo overlay package not found in ${DIST_DIR}" >&2
    exit 1
fi

require_root "${PACKAGE_PATH}"

if ! command -v emerge >/dev/null 2>&1; then
    echo "error: emerge not found; this installer must run on Gentoo" >&2
    exit 1
fi

WORK_DIR="$(mktemp -d "${TMP_ROOT}/spider-crawler-emerge.XXXXXX")"
tar --no-same-owner -xzf "${PACKAGE_PATH}" -C "${WORK_DIR}"

OVERLAY_DIR="$(find_overlay_dir "${WORK_DIR}")"
if [ -z "${OVERLAY_DIR}" ] || [ ! -d "${OVERLAY_DIR}" ]; then
    echo "error: gentoo overlay not found in package: ${PACKAGE_PATH}" >&2
    exit 1
fi

echo "staging directory: ${WORK_DIR}"
echo "gentoo overlay: ${OVERLAY_DIR}"

prepare_overlay "${OVERLAY_DIR}"

run_emerge "${OVERLAY_DIR}"

echo "installed gentoo package: ${ATOM}"
echo "overlay package: ${PACKAGE_PATH}"
