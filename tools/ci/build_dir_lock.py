#!/usr/bin/env python3
"""Run one command under an advisory lock on its canonical build directory.

Validation stages wait for the lock. Interactive builds (``--no-wait``, which
``governed-build.sh`` uses for every ``cmake --build``) refuse instead and name
the process that holds it: two builds racing into one tree corrupt each other's
objects and double the host load, and relaunching a build that is still running
elsewhere is the common way that happens.

The lock is a kernel ``flock``, so it is released the moment its holder exits,
however it exits; a holder that was killed cannot leave a stale lock behind.
The ``.holder`` sidecar only describes who holds it, and is read only when the
lock is actually held. A process holding the lock exports the canonical build
directory in ``PULP_BUILD_DIR_LOCK_HELD`` so its own descendants may build into
that tree without deadlocking or refusing themselves.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import signal
import socket
import stat
import subprocess
import sys
import time
from contextlib import contextmanager
from pathlib import Path
from typing import BinaryIO, Iterator, Sequence


LOCK_ROOT_ENV = "PULP_BUILD_DIR_LOCK_ROOT"
HELD_ENV = "PULP_BUILD_DIR_LOCK_HELD"
LOCK_MARKER_PREFIX = b"pulp-build-dir-lock-v1\n"
BUSY_EXIT = 75


class BuildDirBusy(RuntimeError):
    """Another live process holds the lock on this build directory."""

    def __init__(self, build_dir: Path, holder: dict[str, object] | None) -> None:
        super().__init__(f"build directory is already being built: {build_dir}")
        self.build_dir = build_dir
        self.holder = holder


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
            return Path(local_app_data) / "Pulp" / "State" / "build-dir-locks"
        return (
            Path.home()
            / "AppData"
            / "Local"
            / "Pulp"
            / "State"
            / "build-dir-locks"
        )
    if sys.platform == "darwin":
        return (
            Path.home()
            / "Library"
            / "Application Support"
            / "Pulp"
            / "build-dir-locks"
        )

    state_home = os.environ.get("XDG_STATE_HOME")
    if state_home and Path(state_home).is_absolute():
        return Path(state_home) / "pulp" / "build-dir-locks"
    return Path.home() / ".local" / "state" / "pulp" / "build-dir-locks"


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


def holder_path_for(lock_path: Path) -> Path:
    return lock_path.with_suffix(".holder")


def _held_dirs(env: dict[str, str] | os._Environ[str]) -> list[str]:
    return [entry for entry in env.get(HELD_ENV, "").split(os.pathsep) if entry]


def held_by_ancestor(build_dir: Path) -> bool:
    """True when this process inherited the lock on ``build_dir`` from a parent."""

    return os.fsdecode(_canonical_build_dir(build_dir)) in _held_dirs(os.environ)


def pid_alive(pid: object) -> bool | None:
    if not isinstance(pid, int) or pid <= 0:
        return None
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    except OSError:
        return None
    return True


def read_holder(lock_path: Path) -> dict[str, object] | None:
    try:
        payload = json.loads(holder_path_for(lock_path).read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return payload if isinstance(payload, dict) else None


def _write_holder(lock_path: Path, build_dir: Path, command: Sequence[str]) -> None:
    payload = {
        "pid": os.getpid(),
        "started_epoch": int(time.time()),
        "started_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "host": socket.gethostname(),
        "cwd": os.getcwd(),
        "build_dir": os.fsdecode(_canonical_build_dir(build_dir)),
        "command": list(command),
    }
    target = holder_path_for(lock_path)
    temporary = target.with_suffix(f".holder.{os.getpid()}")
    try:
        temporary.write_text(json.dumps(payload) + "\n", encoding="utf-8")
        os.replace(temporary, target)
    except OSError:
        # Best effort: the lock itself is the guarantee, not its description.
        temporary.unlink(missing_ok=True)


def _clear_holder(lock_path: Path) -> None:
    holder = read_holder(lock_path)
    if holder is not None and holder.get("pid") == os.getpid():
        holder_path_for(lock_path).unlink(missing_ok=True)


def describe_busy(busy: BuildDirBusy) -> str:
    lines = [f"[build-dir-lock] refusing: {busy.build_dir} is already being built."]
    holder = busy.holder
    if holder is None:
        lines.append("[build-dir-lock]   holder: unknown (it did not record itself)")
    else:
        alive = pid_alive(holder.get("pid"))
        state = {True: "alive", False: "exited", None: "unknown"}[alive]
        started = holder.get("started_at", "?")
        epoch = holder.get("started_epoch")
        if isinstance(epoch, int):
            started = f"{started} ({int(time.time()) - epoch}s ago)"
        command = holder.get("command")
        if isinstance(command, list):
            command = " ".join(str(part) for part in command)
        lines += [
            f"[build-dir-lock]   holder pid={holder.get('pid', '?')} ({state}) "
            f"host={holder.get('host', '?')} since {started}",
            f"[build-dir-lock]   cwd: {holder.get('cwd', '?')}",
            f"[build-dir-lock]   command: {command}",
        ]
    lines += [
        "[build-dir-lock] Two builds in one tree race each other's objects and double the",
        "[build-dir-lock] host load. Wait for that build (or attach to where it is running);",
        "[build-dir-lock] do not relaunch it. The lock releases the moment its holder exits.",
    ]
    return "\n".join(lines)


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
def exclusive_build_dir(
    build_dir: Path, *, wait: bool = True, command: Sequence[str] = ()
) -> Iterator[None]:
    """Hold the stable host lock for one canonical configured build directory.

    ``wait=False`` raises :class:`BuildDirBusy` instead of queueing behind a
    live holder. A directory this process inherited from a locking ancestor is
    entered without locking again.
    """

    canonical = _canonical_build_dir(build_dir)
    if held_by_ancestor(build_dir):
        yield
        return
    root = lock_root()
    _ensure_lock_root(root)
    path = lock_path_for(build_dir, root)
    previous_held = os.environ.get(HELD_ENV)
    with _open_lock_file(path) as lock_file:
        if os.name == "nt":
            import msvcrt

            if lock_file.seek(0, os.SEEK_END) == 0:
                lock_file.write(b"\0")
                lock_file.flush()
            lock_file.seek(0)
            try:
                msvcrt.locking(
                    lock_file.fileno(), msvcrt.LK_LOCK if wait else msvcrt.LK_NBLCK, 1
                )
            except OSError:
                if wait:
                    raise
                raise BuildDirBusy(Path(os.fsdecode(canonical)), read_holder(path)) from None

            def unlock() -> None:
                lock_file.seek(0)
                msvcrt.locking(lock_file.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl

            try:
                fcntl.flock(
                    lock_file.fileno(), fcntl.LOCK_EX | (0 if wait else fcntl.LOCK_NB)
                )
            except BlockingIOError:
                raise BuildDirBusy(Path(os.fsdecode(canonical)), read_holder(path)) from None

            def unlock() -> None:
                fcntl.flock(lock_file.fileno(), fcntl.LOCK_UN)

        try:
            _verify_lock_identity(lock_file, canonical)
            _write_holder(path, build_dir, command)
            os.environ[HELD_ENV] = os.pathsep.join(
                [*_held_dirs(os.environ), os.fsdecode(canonical)]
            )
            yield
        finally:
            if previous_held is None:
                os.environ.pop(HELD_ENV, None)
            else:
                os.environ[HELD_ENV] = previous_held
            _clear_holder(path)
            unlock()


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument(
        "--no-wait",
        action="store_true",
        help=f"refuse (exit {BUSY_EXIT}) and name the holder instead of queueing",
    )
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args(argv)
    if args.command[:1] == ["--"]:
        args.command = args.command[1:]
    if not args.command:
        parser.error("a command is required after --")
    return args


def _run_forwarding_signals(command: Sequence[str]) -> int:
    """Run ``command``, passing termination on so the lock never outlives it."""

    child = subprocess.Popen(list(command), shell=False)
    forwarded = (signal.SIGTERM, signal.SIGHUP) if os.name != "nt" else ()
    previous = {}

    def forward(signum: int, _frame: object) -> None:
        try:
            child.send_signal(signum)
        except OSError:
            pass

    for signum in forwarded:
        previous[signum] = signal.signal(signum, forward)
    try:
        try:
            return child.wait()
        except KeyboardInterrupt:
            # The terminal delivered SIGINT to the whole process group. Give the
            # child a moment to stop on its own; the lock is held until it has.
            try:
                child.wait(timeout=10)
            except subprocess.TimeoutExpired:
                child.kill()
                child.wait()
            return 130
    finally:
        for signum, handler in previous.items():
            signal.signal(signum, handler)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        with exclusive_build_dir(args.build_dir, wait=not args.no_wait, command=args.command):
            return _run_forwarding_signals(args.command)
    except BuildDirBusy as busy:
        print(describe_busy(busy), file=sys.stderr)
        return BUSY_EXIT


if __name__ == "__main__":
    raise SystemExit(main())
