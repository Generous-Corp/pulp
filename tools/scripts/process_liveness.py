#!/usr/bin/env python3
"""Is a process still running? One answer for every platform.

The POSIX idiom ``os.kill(pid, 0)`` must never run on Windows. There
``os.kill(pid, 0)`` is ``os.kill(pid, signal.CTRL_C_EVENT)``: Python calls
``GenerateConsoleCtrlEvent``, which sends Ctrl+C to every process sharing the
console instead of testing ``pid``. A liveness probe in a ctest-run script
therefore kills the whole Windows ctest run (``0xC000013A``). On Windows this
module opens the process for ``PROCESS_QUERY_LIMITED_INFORMATION`` and reads its
exit code instead.

``raw_pid_probe_lint.py`` rejects ``os.kill(<pid>, 0)`` anywhere else in the
tree, so a new probe has to come through here.
"""
from __future__ import annotations

import os

_PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
_ERROR_ACCESS_DENIED = 5
_STILL_ACTIVE = 259


def _windows_pid_alive(pid: int) -> bool:
    import ctypes
    from ctypes import wintypes

    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenProcess.restype = wintypes.HANDLE
    handle = kernel32.OpenProcess(_PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
    if not handle:
        # Access denied means the process exists under another account.
        return ctypes.get_last_error() == _ERROR_ACCESS_DENIED
    try:
        code = wintypes.DWORD()
        if not kernel32.GetExitCodeProcess(handle, ctypes.byref(code)):
            return True
        return code.value == _STILL_ACTIVE
    finally:
        kernel32.CloseHandle(handle)


def pid_alive(pid: object, *, platform: str | None = None) -> bool | None:
    """True if ``pid`` names a live process, False if it has exited, None if
    ``pid`` is not a positive int or the platform will not say."""
    if not isinstance(pid, int) or isinstance(pid, bool) or pid <= 0:
        return None
    if (platform or os.name) == "nt":
        return _windows_pid_alive(pid)
    try:
        os.kill(pid, 0)  # raw-pid-probe-lint: skip the one POSIX probe
    except ProcessLookupError:
        return False
    except PermissionError:
        return True  # owned by another user: it exists
    except OSError:
        return None
    return True


__all__ = ["pid_alive"]
