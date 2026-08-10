#!/usr/bin/env python3
"""Immutable, collision-safe evidence files for one generation run."""

from __future__ import annotations

import os
import stat


_RUN_MARKER = ".forge-generation.lock"


class RunDirectory:
    """A generation-owned artifact directory and its optional reservation."""

    def __init__(self, path: str, marker: str | None) -> None:
        self.path = path
        self._marker = marker

    def close(self) -> None:
        if self._marker is None:
            return
        try:
            os.unlink(self._marker)
        except FileNotFoundError:
            pass
        self._marker = None


class ReservedText:
    """An exclusively created text artifact kept open across a paid call."""

    def __init__(self, path: str, descriptor: int) -> None:
        self.path = path
        self._descriptor = descriptor

    def write(self, content: str) -> str:
        descriptor = self._descriptor
        if descriptor is None:
            raise RuntimeError(f"artifact reservation is already closed: {self.path}")
        self._descriptor = None
        try:
            with os.fdopen(descriptor, "w", encoding="utf-8") as output:
                output.write(content)
                output.flush()
                os.fsync(output.fileno())
        except BaseException:
            try:
                os.close(descriptor)
            except OSError:
                pass
            raise
        return self.path

    def close(self) -> None:
        if self._descriptor is None:
            return
        try:
            os.close(self._descriptor)
        finally:
            self._descriptor = None


def begin_run(base: str) -> RunDirectory:
    """Atomically reserve a writable namespace before any provider call."""
    os.makedirs(base, mode=0o700, exist_ok=True)
    occupied = any(name.startswith("attempt") for name in os.listdir(base))
    marker = os.path.join(base, _RUN_MARKER)
    if not occupied:
        try:
            descriptor = os.open(
                marker, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        except FileExistsError:
            pass
        else:
            os.close(descriptor)
            return RunDirectory(base, marker)

    run = 2
    while True:
        candidate = os.path.join(base, f"run{run:02d}")
        try:
            os.mkdir(candidate, mode=0o700)
        except FileExistsError:
            run += 1
            continue
        return RunDirectory(candidate, None)


def reserve_text(directory: str, filename: str) -> ReservedText:
    """Create and hold one immutable artifact path, proving it is writable."""
    os.makedirs(directory, mode=0o700, exist_ok=True)
    path = os.path.join(directory, filename)
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    mode = os.fstat(descriptor).st_mode
    if not stat.S_ISREG(mode):
        os.close(descriptor)
        raise OSError(f"artifact path is not a regular file: {path}")
    return ReservedText(path, descriptor)


def retain_text(directory: str, filename: str, content: str) -> str:
    """Exclusively create, flush, and retain one text artifact."""
    artifact = reserve_text(directory, filename)
    try:
        return artifact.write(content)
    finally:
        artifact.close()
