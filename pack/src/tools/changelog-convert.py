#!/usr/bin/env python3
from __future__ import annotations

import argparse
import email.utils
import re
import sys
from dataclasses import dataclass
from datetime import datetime, timezone, timedelta
from pathlib import Path


MAINTAINER = "DingJing <dingjing@live.cn>"
PACKAGE_NAME = "spider-crawler"
TZ = timezone(timedelta(hours=8))


@dataclass
class Entry:
    version: str
    timestamp: datetime
    items: list[str]


def parse_timestamp(value: str, line_no: int) -> datetime:
    value = value.strip()
    for fmt in ("%Y-%m-%d %H:%M:%S", "%Y-%m-%d"):
        try:
            parsed = datetime.strptime(value, fmt)
            if fmt == "%Y-%m-%d":
                parsed = parsed.replace(hour=12, minute=0, second=0)
            return parsed.replace(tzinfo=TZ)
        except ValueError:
            pass
    raise ValueError(f"line {line_no}: invalid changelog date: {value}")


def parse_changelog(path: Path) -> list[Entry]:
    header_re = re.compile(r"^##\s+([^\s]+)\s+--\s+(.+?)\s*$")
    item_re = re.compile(r"^\d+\.\s+(.+?)\s*$")
    entries: list[Entry] = []
    current: Entry | None = None

    for line_no, raw_line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw_line.strip()
        if not line:
            continue

        header = header_re.match(line)
        if header:
            if current is not None:
                if not current.items:
                    raise ValueError(f"line {line_no}: version {current.version} has no changelog items")
                entries.append(current)
            current = Entry(
                version=header.group(1),
                timestamp=parse_timestamp(header.group(2), line_no),
                items=[],
            )
            continue

        item = item_re.match(line)
        if item:
            if current is None:
                raise ValueError(f"line {line_no}: changelog item appears before version header")
            current.items.append(item.group(1))
            continue

        raise ValueError(f"line {line_no}: unsupported changelog syntax: {raw_line}")

    if current is not None:
        if not current.items:
            raise ValueError(f"version {current.version} has no changelog items")
        entries.append(current)

    if not entries:
        raise ValueError("changelog has no entries")

    return entries


def render_deb(entries: list[Entry]) -> str:
    parts: list[str] = []
    for entry in entries:
        parts.append(f"{PACKAGE_NAME} ({entry.version}) stable; urgency=medium")
        parts.append("")
        for item in entry.items:
            parts.append(f"  * {item}")
        parts.append("")
        parts.append(f" -- {MAINTAINER}  {email.utils.format_datetime(entry.timestamp)}")
        parts.append("")
    return "\n".join(parts)


def render_rpm(entries: list[Entry]) -> str:
    parts = ["%changelog"]
    for entry in entries:
        parts.append(f"* {entry.timestamp.strftime('%a %b %d %Y')} {MAINTAINER} - {entry.version}-1")
        for item in entry.items:
            parts.append(f"- {item}")
        parts.append("")
    return "\n".join(parts)


def render_gentoo(entries: list[Entry]) -> str:
    parts = [
        f"# ChangeLog for app-admin/{PACKAGE_NAME}",
        f"# Generated from pack/src/changelog.md by {PACKAGE_NAME} packaging.",
        "",
    ]
    for entry in entries:
        date_text = entry.timestamp.strftime("%d %b %Y")
        parts.append(f"*{PACKAGE_NAME}-{entry.version} ({date_text})")
        parts.append("")
        parts.append(f"  {date_text}; {MAINTAINER}:")
        for item in entry.items:
            parts.append(f"  - {item}")
        parts.append("")
    return "\n".join(parts)


def main() -> int:
    parser = argparse.ArgumentParser(description="Convert markdown changelog to package changelog formats")
    parser.add_argument("format", choices=("deb", "rpm", "gentoo"))
    parser.add_argument("input", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()

    try:
        entries = parse_changelog(args.input)
        if args.format == "deb":
            output = render_deb(entries)
        elif args.format == "rpm":
            output = render_rpm(entries)
        else:
            output = render_gentoo(entries)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(output + "\n", encoding="utf-8")
    except Exception as error:
        print(f"error: {error}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
