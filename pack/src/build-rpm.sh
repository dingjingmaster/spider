#!/bin/sh
set -eu

ARCH="${1:-$(uname -m)}"
VERSION="${2:-0.1.0}"
PACK_DIR="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
DIST_DIR="${PACK_DIR}/dist"
ROOTFS_DIR="${PACK_DIR}/build/rootfs"
POSTINSTALL_SRC="${PACK_DIR}/src/scripts/general-postinstall.sh"
POSTREMOVE_SRC="${PACK_DIR}/src/scripts/general-postremove.sh"
CHANGELOG_SRC="${PACK_DIR}/src/changelog.md"
CHANGELOG_CONVERT="${PACK_DIR}/src/tools/changelog-convert.py"
RPMBUILD_DIR="${PACK_DIR}/build/package/rpm"
PACKAGE_NAME="spider-crawler"
SOURCE_NAME="${PACKAGE_NAME}-${VERSION}"
SOURCE_TAR="${RPMBUILD_DIR}/SOURCES/${SOURCE_NAME}.tar.gz"
SPEC_FILE="${RPMBUILD_DIR}/SPECS/${PACKAGE_NAME}.spec"
RPM_CHANGELOG="${RPMBUILD_DIR}/SPECS/${PACKAGE_NAME}.changelog"
PACKAGE_PATH="${DIST_DIR}/${PACKAGE_NAME}-${VERSION}-${ARCH}.rpm"

case "${ARCH}" in
    amd64)
        RPM_ARCH="x86_64"
        ;;
    arm64)
        RPM_ARCH="aarch64"
        ;;
    *)
        RPM_ARCH="${ARCH}"
        ;;
esac

if [ ! -d "${ROOTFS_DIR}" ]; then
    echo "error: rootfs not found: ${ROOTFS_DIR}" >&2
    exit 1
fi

if [ ! -x "${ROOTFS_DIR}/opt/spider/bin/spider" ]; then
    echo "error: spider binary not found in rootfs" >&2
    exit 1
fi

if [ ! -f "${POSTINSTALL_SRC}" ]; then
    echo "error: postinstall script not found: ${POSTINSTALL_SRC}" >&2
    exit 1
fi

if [ ! -f "${POSTREMOVE_SRC}" ]; then
    echo "error: postremove script not found: ${POSTREMOVE_SRC}" >&2
    exit 1
fi

if [ ! -f "${CHANGELOG_SRC}" ]; then
    echo "error: changelog not found: ${CHANGELOG_SRC}" >&2
    exit 1
fi

if [ ! -f "${CHANGELOG_CONVERT}" ]; then
    echo "error: changelog converter not found: ${CHANGELOG_CONVERT}" >&2
    exit 1
fi

rm -rf "${RPMBUILD_DIR}"
mkdir -p "${RPMBUILD_DIR}/BUILD" "${RPMBUILD_DIR}/BUILDROOT" "${RPMBUILD_DIR}/RPMS" \
    "${RPMBUILD_DIR}/SOURCES/${SOURCE_NAME}" "${RPMBUILD_DIR}/SPECS" "${RPMBUILD_DIR}/SRPMS" \
    "${RPMBUILD_DIR}/tmp" "${DIST_DIR}"

cp -a "${ROOTFS_DIR}/." "${RPMBUILD_DIR}/SOURCES/${SOURCE_NAME}/"
tar -C "${RPMBUILD_DIR}/SOURCES" -czf "${SOURCE_TAR}" "${SOURCE_NAME}"
rm -rf "${RPMBUILD_DIR}/SOURCES/${SOURCE_NAME}"

cat > "${SPEC_FILE}" <<EOF_SPEC
%global debug_package %{nil}
%global __strip /bin/true
%global _build_id_links none

Name: ${PACKAGE_NAME}
Version: ${VERSION}
Release: 1%{?dist}
Summary: Spider crawler service with local web dashboard
License: all-rights-reserved
BuildArch: ${RPM_ARCH}
Source0: %{name}-%{version}.tar.gz
Requires(post): shadow-utils

%description
Spider is a C crawler daemon that stores tasks and results in SQLite and
serves a local web dashboard on 127.0.0.1:8080.

%prep
%setup -q

%build

%install
rm -rf %{buildroot}
mkdir -p %{buildroot}
cp -a . %{buildroot}/

%check
test -x %{buildroot}/opt/spider/bin/spider
test -f %{buildroot}/usr/lib/systemd/system/spider.service
test -f %{buildroot}/opt/spider/share/spider/ui/index.html

%post -p /bin/bash
EOF_SPEC

sed '1{/^#!/d;}' "${POSTINSTALL_SRC}" >> "${SPEC_FILE}"

cat >> "${SPEC_FILE}" <<'EOF_SPEC'

%postun -p /bin/bash
if [ "$1" = "0" ]; then
EOF_SPEC

sed '1{/^#!/d;}' "${POSTREMOVE_SRC}" | sed 's/^/    /' >> "${SPEC_FILE}"

cat >> "${SPEC_FILE}" <<'EOF_SPEC'
fi

%files
%attr(0755,root,root) %dir /opt/spider
%attr(0755,root,root) %dir /opt/spider/bin
%attr(0755,root,root) /opt/spider/bin/spider
%attr(0755,root,root) %dir /opt/spider/share
%attr(0755,root,root) %dir /opt/spider/share/spider
%attr(0755,root,root) %dir /opt/spider/share/spider/ui
%attr(0644,root,root) /opt/spider/share/spider/ui/*
%attr(0644,root,root) /usr/lib/systemd/system/spider.service
/usr/bin/spider
EOF_SPEC

python3 "${CHANGELOG_CONVERT}" rpm "${CHANGELOG_SRC}" "${RPM_CHANGELOG}"
cat "${RPM_CHANGELOG}" >> "${SPEC_FILE}"

rpmbuild --define "_topdir ${RPMBUILD_DIR}" --define "_tmppath ${RPMBUILD_DIR}/tmp" -bb "${SPEC_FILE}"

RPM_OUTPUT="$(find "${RPMBUILD_DIR}/RPMS" -type f -name "${PACKAGE_NAME}-${VERSION}-*.rpm" -print | sort | tail -n 1)"
if [ -z "${RPM_OUTPUT}" ] || [ ! -f "${RPM_OUTPUT}" ]; then
    echo "error: rpm output not found" >&2
    exit 1
fi

cp "${RPM_OUTPUT}" "${PACKAGE_PATH}"

echo "rpm package: ${PACKAGE_PATH}"
