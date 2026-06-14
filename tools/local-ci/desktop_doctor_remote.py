"""Compatibility facade for remote desktop doctor check builders."""

from __future__ import annotations

from desktop_doctor_remote_linux import linux_remote_doctor_checks
from desktop_doctor_remote_ssh import ssh_desktop_doctor_checks
from desktop_doctor_remote_windows import windows_session_doctor_checks


__all__ = (
    "linux_remote_doctor_checks",
    "ssh_desktop_doctor_checks",
    "windows_session_doctor_checks",
)
