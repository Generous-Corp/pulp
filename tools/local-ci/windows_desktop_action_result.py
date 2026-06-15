"""Compatibility facade for Windows desktop action result helpers."""

from __future__ import annotations

from windows_desktop_action_artifacts import attach_windows_before_diff_artifacts, attach_windows_ui_snapshot
from windows_desktop_action_fetch import fetch_windows_session_agent_outputs
from windows_desktop_action_interaction import attach_windows_interaction_summary
from windows_desktop_action_manifest import build_windows_desktop_action_manifest
from windows_desktop_action_wait import wait_for_windows_session_agent_manifest


__all__ = (
    "attach_windows_before_diff_artifacts",
    "attach_windows_interaction_summary",
    "attach_windows_ui_snapshot",
    "build_windows_desktop_action_manifest",
    "fetch_windows_session_agent_outputs",
    "wait_for_windows_session_agent_manifest",
)
