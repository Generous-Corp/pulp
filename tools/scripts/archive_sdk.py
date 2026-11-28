#!/usr/bin/env python3
"""Create a reproducible gzip-compressed SDK tar archive."""

from __future__ import annotations

import argparse
import gzip
import tarfile
from pathlib import Path


def archive_tree(source: Path, output: Path, epoch: int) -> None:
    if not source.is_dir():
        raise ValueError(f"SDK staging directory does not exist: {source}")
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("wb") as raw, gzip.GzipFile(filename="", fileobj=raw, mode="wb", mtime=epoch) as compressed:
        with tarfile.open(fileobj=compressed, mode="w|", format=tarfile.PAX_FORMAT) as archive:
            paths = [source, *sorted(source.rglob("*"), key=lambda path: path.relative_to(source).as_posix())]
            for path in paths:
                info = archive.gettarinfo(str(path), arcname=path.name if path == source else f"{source.name}/{path.relative_to(source).as_posix()}")
                info.mtime = epoch
                info.uid = info.gid = 0
                info.uname = info.gname = ""
                if path.is_file() and not path.is_symlink():
                    with path.open("rb") as member:
                        archive.addfile(info, member)
                else:
                    archive.addfile(info)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--epoch", type=int, required=True)
    args = parser.parse_args()
    archive_tree(args.source, args.output, args.epoch)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
