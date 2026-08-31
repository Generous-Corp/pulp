"""Portable nonblocking file locking for Forge Rack qualification receipts."""

from __future__ import annotations

import errno
import os

try:
    import fcntl
except ImportError:  # pragma: no cover - exercised by the Windows CI lane
    fcntl = None

try:
    import msvcrt
except ImportError:  # pragma: no cover - exercised by the POSIX CI lanes
    msvcrt = None


def exclusive_nonblocking(fd: int) -> None:
    """Claim an open descriptor or raise ``BlockingIOError`` on contention."""
    if fcntl is not None:
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        return
    if msvcrt is None:
        raise RuntimeError("this platform has no supported file-lock API")
    if os.fstat(fd).st_size == 0:
        os.write(fd, b"\0")
    os.lseek(fd, 0, os.SEEK_SET)
    try:
        msvcrt.locking(fd, msvcrt.LK_NBLCK, 1)
    except OSError as exc:
        if exc.errno in {errno.EACCES, errno.EAGAIN, errno.EDEADLK}:
            raise BlockingIOError from exc
        raise
