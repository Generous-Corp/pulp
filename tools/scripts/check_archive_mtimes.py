#!/usr/bin/env python3
"""Reject archives containing entries newer than a supplied Unix timestamp."""

from __future__ import annotations

import argparse
import tarfile
import time
import zipfile
from pathlib import Path


def member_mtimes(archive: Path) -> list[tuple[str, float]]:
    if tarfile.is_tarfile(archive):
        with tarfile.open(archive, "r:*") as tar:
            return [(member.name, member.mtime) for member in tar.getmembers()]
    if zipfile.is_zipfile(archive):
        with zipfile.ZipFile(archive) as zipped:
            return [
                (member.filename, time.mktime(member.date_time + (0, 0, -1)))
                for member in zipped.infolist()
            ]
    raise ValueError(f"unsupported archive format: {archive}")


def check_archive(archive: Path, maximum: float) -> list[tuple[str, float]]:
    return [(name, mtime) for name, mtime in member_mtimes(archive) if mtime > maximum]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--max-mtime", type=float, default=time.time())
    args = parser.parse_args()
    future = check_archive(args.archive, args.max_mtime)
    if future:
        for name, mtime in future:
            print(f"future mtime {mtime:.0f} > {args.max_mtime:.0f}: {name}")
        return 1
    print(f"OK: {args.archive} ({len(member_mtimes(args.archive))} entries)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
