#!/usr/bin/env python3
"""Run one command under an advisory lock shared by all validation stages."""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from contextlib import contextmanager
from pathlib import Path
from typing import Iterator, Sequence


@contextmanager
def exclusive_build_dir(build_dir: Path) -> Iterator[None]:
    """Hold the repository-local lock for one configured build directory."""

    resolved = build_dir.resolve(strict=False)
    resolved.parent.mkdir(parents=True, exist_ok=True)
    lock_path = resolved.parent / f".{resolved.name}.pulp-validation.lock"
    with lock_path.open("a+b") as lock_file:
        if os.name == "nt":
            import msvcrt

            if lock_file.seek(0, os.SEEK_END) == 0:
                lock_file.write(b"\0")
                lock_file.flush()
            lock_file.seek(0)
            msvcrt.locking(lock_file.fileno(), msvcrt.LK_LOCK, 1)
            try:
                yield
            finally:
                lock_file.seek(0)
                msvcrt.locking(lock_file.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl

            fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    if args.command[:1] == ["--"]:
        args.command = args.command[1:]
    if not args.command:
        parser.error("a command is required after --")
    return args


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    with exclusive_build_dir(args.build_dir):
        return subprocess.run(args.command, shell=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
