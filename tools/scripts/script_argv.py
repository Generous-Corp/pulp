#!/usr/bin/env python3
"""The argv that runs a program, including a shebang script on Windows.

Windows cannot execute a ``#!`` script directly: ``CreateProcess`` answers
``[WinError 193] %1 is not a valid Win32 application``. Tests stand in for
native tools (trace_processor, gh, a producer binary) with small Python or
shell scripts, and the code under test launches whatever path it is given, so
on Windows every such launch failed before the code under test ran.

``argv_for`` returns ``[path]`` everywhere except Windows, where a file that
starts with ``#!`` (and is not a PE image) is run through its interpreter:
Python through this interpreter, a shell script through ``bash`` when one is on
``PATH``. Native executables pass through unchanged, so production launches of
real tools behave exactly as before on every platform.
"""
from __future__ import annotations

import os
import shutil
import sys


def _interpreter(shebang: bytes) -> str:
    """The interpreter a ``#!`` line names: ``python3`` for ``#!/usr/bin/env python3``."""
    words = shebang[2:].strip().split()
    if not words:
        return ""
    def basename(word: bytes) -> bytes:
        # A Windows interpreter path uses backslashes and ends in .exe.
        name = word.replace(b"\\", b"/").rsplit(b"/", 1)[-1].lower()
        return name[:-4] if name.endswith(b".exe") else name

    name = basename(words[0])
    if name == b"env" and len(words) > 1:
        name = basename(words[1])
    return name.decode("ascii", "replace")


def argv_for(executable: str | os.PathLike[str], *, platform: str | None = None) -> list[str]:
    """The argv prefix that launches ``executable`` on ``platform`` (default: this one)."""
    path = os.fspath(executable)
    if (platform or os.name) != "nt":
        return [path]
    try:
        with open(path, "rb") as handle:
            head = handle.read(256)
    except OSError:
        return [path]
    if not head.startswith(b"#!"):
        return [path]  # a native image, or a file the OS will judge itself
    interpreter = _interpreter(head.split(b"\n", 1)[0])
    if interpreter.startswith("python"):
        return [sys.executable, path]
    if interpreter in ("sh", "bash", "dash", "zsh"):
        bash = _windows_bash()
        if bash:
            return [bash, path]
    return [path]


def _windows_bash() -> str | None:
    """Git for Windows' bash. PATH often carries only ``Git\\cmd`` (where
    ``git.exe`` lives), so look beside it for ``Git\\bin\\bash.exe`` when
    ``bash`` itself is not on PATH."""
    bash = shutil.which("bash")
    if bash:
        return bash
    git = shutil.which("git")
    if not git:
        return None
    root = os.path.dirname(os.path.dirname(git))
    for candidate in (os.path.join(root, "bin", "bash.exe"),
                      os.path.join(root, "usr", "bin", "bash.exe")):
        if os.path.isfile(candidate):
            return candidate
    return None


__all__ = ["argv_for"]
