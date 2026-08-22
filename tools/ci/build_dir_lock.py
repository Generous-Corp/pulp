#!/usr/bin/env python3
"""Run one command under an advisory lock shared by all validation stages."""

from __future__ import annotations

import argparse
import hashlib
import os
import stat
import subprocess
import sys
from contextlib import contextmanager
from pathlib import Path
from typing import BinaryIO, Iterator, Sequence


LOCK_ROOT_ENV = "PULP_BUILD_DIR_LOCK_ROOT"
LOCK_MARKER_PREFIX = b"pulp-build-dir-lock-v1\n"


def _canonical_build_dir(build_dir: Path) -> bytes:
    resolved = build_dir.expanduser().resolve(strict=False)
    normalized = os.path.normcase(str(resolved))
    return os.fsencode(normalized)


def lock_root() -> Path:
    """Return stable per-user host state outside repository checkouts."""

    override = os.environ.get(LOCK_ROOT_ENV)
    if override:
        candidate = Path(override).expanduser()
        if not candidate.is_absolute():
            raise ValueError(f"{LOCK_ROOT_ENV} must be an absolute path")
        return candidate.resolve(strict=False)

    if os.name == "nt":
        local_app_data = os.environ.get("LOCALAPPDATA")
        if local_app_data:
            return Path(local_app_data) / "Pulp" / "build-dir-locks"
        return Path.home() / "AppData" / "Local" / "Pulp" / "build-dir-locks"
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Caches" / "Pulp" / "build-dir-locks"

    runtime_dir = os.environ.get("XDG_RUNTIME_DIR")
    if runtime_dir and Path(runtime_dir).is_absolute():
        return Path(runtime_dir) / "pulp" / "build-dir-locks"
    cache_home = os.environ.get("XDG_CACHE_HOME")
    if cache_home and Path(cache_home).is_absolute():
        return Path(cache_home) / "pulp" / "build-dir-locks"
    return Path.home() / ".cache" / "pulp" / "build-dir-locks"


def _ensure_lock_root(root: Path) -> None:
    root.mkdir(mode=0o700, parents=True, exist_ok=True)
    root_stat = root.lstat()
    if stat.S_ISLNK(root_stat.st_mode) or not stat.S_ISDIR(root_stat.st_mode):
        raise OSError(f"build-directory lock root is not a real directory: {root}")
    if os.name != "nt":
        if root_stat.st_uid != os.geteuid():
            raise PermissionError(f"build-directory lock root is not owned by this user: {root}")
        if stat.S_IMODE(root_stat.st_mode) != 0o700:
            root.chmod(0o700)


def lock_path_for(build_dir: Path, root: Path | None = None) -> Path:
    canonical = _canonical_build_dir(build_dir)
    digest = hashlib.sha256(canonical).hexdigest()
    return (root if root is not None else lock_root()) / f"build-dir-{digest}.lock"


def _open_lock_file(lock_path: Path) -> BinaryIO:
    flags = os.O_RDWR | os.O_CREAT
    flags |= getattr(os, "O_CLOEXEC", 0)
    flags |= getattr(os, "O_NOFOLLOW", 0)
    descriptor = os.open(lock_path, flags, 0o600)
    try:
        file_stat = os.fstat(descriptor)
        if not stat.S_ISREG(file_stat.st_mode):
            raise OSError(f"build-directory lock is not a regular file: {lock_path}")
        if os.name != "nt":
            if file_stat.st_uid != os.geteuid():
                raise PermissionError(
                    f"build-directory lock is not owned by this user: {lock_path}"
                )
            if stat.S_IMODE(file_stat.st_mode) != 0o600:
                os.fchmod(descriptor, 0o600)
        return os.fdopen(descriptor, "r+b", closefd=True)
    except BaseException:
        os.close(descriptor)
        raise


def _verify_lock_identity(lock_file: BinaryIO, canonical: bytes) -> None:
    marker = LOCK_MARKER_PREFIX + hashlib.sha512(canonical).hexdigest().encode("ascii") + b"\n"
    lock_file.seek(0)
    existing = lock_file.read()
    if not existing or existing == b"\0":
        lock_file.seek(0)
        lock_file.truncate()
        lock_file.write(marker)
        lock_file.flush()
        os.fsync(lock_file.fileno())
    elif existing != marker:
        raise OSError("build-directory lock identity collision or corrupt lock state")


@contextmanager
def exclusive_build_dir(build_dir: Path) -> Iterator[None]:
    """Hold the stable host lock for one canonical configured build directory."""

    canonical = _canonical_build_dir(build_dir)
    root = lock_root()
    _ensure_lock_root(root)
    path = lock_path_for(build_dir, root)
    with _open_lock_file(path) as lock_file:
        if os.name == "nt":
            import msvcrt

            if lock_file.seek(0, os.SEEK_END) == 0:
                lock_file.write(b"\0")
                lock_file.flush()
            lock_file.seek(0)
            msvcrt.locking(lock_file.fileno(), msvcrt.LK_LOCK, 1)
            try:
                _verify_lock_identity(lock_file, canonical)
                yield
            finally:
                lock_file.seek(0)
                msvcrt.locking(lock_file.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl

            fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX)
            try:
                _verify_lock_identity(lock_file, canonical)
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
