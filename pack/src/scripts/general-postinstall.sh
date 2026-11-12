#!/bin/bash
set -e

SPIDER_USER="spider"
SPIDER_HOME="/var/lib/spider"

if [ -x /usr/sbin/nologin ]; then
    SPIDER_SHELL="/usr/sbin/nologin"
elif [ -x /sbin/nologin ]; then
    SPIDER_SHELL="/sbin/nologin"
else
    SPIDER_SHELL="/bin/false"
fi

if ! getent group "${SPIDER_USER}" >/dev/null 2>&1; then
    groupadd -r "${SPIDER_USER}" 2>/dev/null || true
fi

if ! getent passwd "${SPIDER_USER}" >/dev/null 2>&1; then
    useradd -r -g "${SPIDER_USER}" -d "${SPIDER_HOME}" -s "${SPIDER_SHELL}" \
        -c "Spider crawler" "${SPIDER_USER}" 2>/dev/null || true
fi

install -d -o "${SPIDER_USER}" -g "${SPIDER_USER}" -m 0750 "${SPIDER_HOME}" 2>/dev/null || \
    mkdir -p "${SPIDER_HOME}" 2>/dev/null || true
chown "${SPIDER_USER}:${SPIDER_USER}" "${SPIDER_HOME}" 2>/dev/null || true
chmod 0750 "${SPIDER_HOME}" 2>/dev/null || true

if command -v systemctl >/dev/null 2>&1; then
    systemctl daemon-reload >/dev/null 2>&1 || true
    systemctl enable spider.service >/dev/null 2>&1 || true
fi

exit 0
