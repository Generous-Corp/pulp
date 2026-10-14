#!/usr/bin/env python3
"""Compute the digests tools/deps/manifest.json pins for a wgpu-native asset.

Each wgpu-native release asset is pinned twice:

- ``sha256`` — the downloaded ``.zip`` itself; setup.sh and configure refuse a
  download that does not match.
- ``tree_sha256`` — the extracted tree: SHA-256 over one ``"<path> <sha256>\\n"``
  line per regular file, sorted by path. The shared source cache keeps only the
  extracted files, so configure re-checks a cache directory against this value
  (``pulp_wgpu_native_tree_sha256`` in tools/cmake/PulpWgpuNative.cmake computes
  the same thing from disk).

Bumping wgpu-native: download every asset the manifest lists into a new, empty
directory, then run

    python3 -I tools/deps/wgpu_native_digests.py <dir>/*.zip

and copy the two digests per asset into the manifest's ``artifacts`` list. The
archives are read in memory; nothing is extracted.
"""

from __future__ import annotations

import hashlib
import json
import sys
import zipfile
from pathlib import Path


def archive_sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def tree_sha256_from_files(files: dict[str, bytes]) -> str:
    lines = "".join(
        f"{name} {hashlib.sha256(data).hexdigest()}\n" for name, data in sorted(files.items())
    )
    return hashlib.sha256(lines.encode("utf-8")).hexdigest()


def tree_sha256_from_zip(path: Path) -> str:
    files: dict[str, bytes] = {}
    with zipfile.ZipFile(path) as archive:
        for info in archive.infolist():
            if info.is_dir():
                continue
            name = info.filename.replace("\\", "/").lstrip("/")
            files[name] = archive.read(info)
    return tree_sha256_from_files(files)


def tree_sha256_from_dir(root: Path) -> str:
    files = {
        p.relative_to(root).as_posix(): p.read_bytes()
        for p in root.rglob("*")
        if p.is_file() and not p.is_symlink()
    }
    return tree_sha256_from_files(files)


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__, file=sys.stderr)
        return 2
    for arg in argv:
        path = Path(arg)
        print(json.dumps({
            "name": path.name.removesuffix(".zip"),
            "sha256": archive_sha256(path),
            "tree_sha256": tree_sha256_from_zip(path),
        }))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
