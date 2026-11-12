#!/bin/bash
set -e

if command -v systemctl >/dev/null 2>&1; then
    systemctl stop spider.service >/dev/null 2>&1 || true
    systemctl disable spider.service >/dev/null 2>&1 || true
    systemctl daemon-reload >/dev/null 2>&1 || true
fi

# Keep /var/lib/spider and the spider user so crawler data survives removal.

exit 0
