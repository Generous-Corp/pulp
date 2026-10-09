"""One Forge generation at a time per output directory.

A generation installs into a Rack plugin directory (and a module generation
rewrites the module pack), so two at once against the same directory overwrite
each other's work. The guard is an exclusive OS file lock held for the life of
the generating process. It is not a marker file: the kernel drops the lock when
the process exits for any reason, a crash or SIGKILL included, so nothing stale
is ever left to clean up.

The lock file is keyed by the directory rather than placed inside it, so taking
the lock never creates or writes into a user's Rack plugin directory:

    <lock root>/<first 32 hex of sha256(realpath(directory))>.lock

The lock root is FORGE_GENERATION_LOCK_DIR when set, else the per-user cache
(`~/Library/Caches/Forge Modular/generation-locks` on macOS,
`$XDG_CACHE_HOME/forge-modular/generation-locks` elsewhere). The Forge app
takes the same lock on the same path, so an app generation and a command-line
run exclude each other.
"""

from __future__ import annotations

import hashlib
import os
import sys

import file_lock

LOCK_DIR_ENV = "FORGE_GENERATION_LOCK_DIR"
# EX_TEMPFAIL: the directory is busy, try again later.
BUSY_EXIT = 75

_held: dict[str, int] = {}


class GenerationBusy(RuntimeError):
    """Another process holds the generation lock on this directory."""


def lock_root() -> str:
    override = os.environ.get(LOCK_DIR_ENV, "").strip()
    if override:
        return os.path.abspath(os.path.expanduser(override))
    if sys.platform == "darwin":
        return os.path.expanduser(
            "~/Library/Caches/Forge Modular/generation-locks")
    cache = os.environ.get("XDG_CACHE_HOME", "").strip() or os.path.expanduser("~/.cache")
    return os.path.join(cache, "forge-modular", "generation-locks")


def lock_path(directory: str) -> str:
    key = hashlib.sha256(os.path.realpath(directory).encode("utf-8", "surrogateescape"))
    return os.path.join(lock_root(), key.hexdigest()[:32] + ".lock")


def acquire(directory: str) -> None:
    """Hold the generation lock on `directory` until this process exits.

    Raises GenerationBusy when another process holds it. Acquiring a directory
    this process already holds is a no-op.
    """
    path = lock_path(directory)
    if path in _held:
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o600)
    try:
        file_lock.exclusive_nonblocking(fd)
    except BlockingIOError:
        os.close(fd)
        raise GenerationBusy(path) from None
    except BaseException:
        os.close(fd)
        raise
    if file_lock.fcntl is not None:
        # For a person reading the lock directory; the lock is the flock, not
        # this text. (Windows locks the file's first byte, so it is left alone.)
        os.ftruncate(fd, 0)
        os.write(fd, f"{os.getpid()} {os.path.realpath(directory)}\n".encode(
            "utf-8", "surrogateescape"))
    _held[path] = fd


def acquire_or_exit(directory: str) -> None:
    """acquire(), or end the process with BUSY_EXIT and a plain reason."""
    try:
        acquire(directory)
    except GenerationBusy:
        print(f"another Forge generation is already running against "
              f"{os.path.realpath(directory)}; wait for it to finish. Two at "
              f"once would overwrite each other's work.", flush=True)
        raise SystemExit(BUSY_EXIT) from None
