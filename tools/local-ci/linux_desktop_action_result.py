"""Compatibility facade for Linux desktop action result helpers."""

from __future__ import annotations

from linux_desktop_action_artifacts import attach_linux_before_diff_artifacts, attach_linux_ui_snapshot
from linux_desktop_action_fetch import fetch_linux_remote_action_outputs
from linux_desktop_action_interaction import attach_linux_interaction_summary
from linux_desktop_action_manifest import build_linux_desktop_action_manifest
from linux_desktop_action_metadata import attach_linux_window_metadata, read_linux_pid_file


__all__ = (
    "attach_linux_before_diff_artifacts",
    "attach_linux_interaction_summary",
    "attach_linux_ui_snapshot",
    "attach_linux_window_metadata",
    "build_linux_desktop_action_manifest",
    "fetch_linux_remote_action_outputs",
    "read_linux_pid_file",
)
