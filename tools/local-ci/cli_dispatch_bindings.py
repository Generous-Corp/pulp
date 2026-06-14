"""Bindings from the local_ci facade to CLI dispatch helpers."""

from __future__ import annotations

from collections.abc import Callable, Mapping
from typing import Any


def _binding(bindings: Mapping[str, Any], name: str) -> Any:
    return bindings[name]


def cmd_desktop_config(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_config(
        args,
        commands={
            "show": _binding(bindings, "cmd_desktop_config_show"),
            "set": _binding(bindings, "cmd_desktop_config_set"),
        },
    )


def cmd_desktop(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_cli_dispatch").dispatch_desktop_command(
        args,
        commands={
            "install": _binding(bindings, "cmd_desktop_install"),
            "doctor": _binding(bindings, "cmd_desktop_doctor"),
            "video-doctor": _binding(bindings, "cmd_desktop_video_doctor"),
            "video-setup": _binding(bindings, "cmd_desktop_video_setup"),
            "status": _binding(bindings, "cmd_desktop_status"),
            "config": _binding(bindings, "cmd_desktop_config"),
            "recent": _binding(bindings, "cmd_desktop_recent"),
            "proof": _binding(bindings, "cmd_desktop_proof"),
            "publish": _binding(bindings, "cmd_desktop_publish"),
            "verdict": _binding(bindings, "cmd_desktop_verdict"),
            "review-issue": _binding(bindings, "cmd_desktop_review_issue"),
            "review-status": _binding(bindings, "cmd_desktop_review_status"),
            "compose-video": _binding(bindings, "cmd_desktop_compose_video"),
            "video-matrix": _binding(bindings, "cmd_desktop_video_matrix"),
            "serve": _binding(bindings, "cmd_desktop_serve"),
            "video": _binding(bindings, "cmd_desktop_video"),
            "cleanup": _binding(bindings, "cmd_desktop_cleanup"),
            "smoke": _binding(bindings, "cmd_desktop_smoke"),
            "click": _binding(bindings, "cmd_desktop_click"),
            "inspect": _binding(bindings, "cmd_desktop_inspect"),
        },
    )


def dispatch_main_command(bindings: Mapping[str, Any], args: Any, print_help: Callable[[], None]) -> int:
    return _binding(bindings, "_cli_dispatch").dispatch_main_command(
        args,
        commands={
            "enqueue": _binding(bindings, "cmd_enqueue"),
            "drain": _binding(bindings, "cmd_drain"),
            "run": _binding(bindings, "cmd_run"),
            "ship": _binding(bindings, "cmd_ship"),
            "check": _binding(bindings, "cmd_check"),
            "list": _binding(bindings, "cmd_list"),
            "bump": _binding(bindings, "cmd_bump"),
            "cancel": _binding(bindings, "cmd_cancel"),
            "logs": _binding(bindings, "cmd_logs"),
            "cleanup": _binding(bindings, "cmd_cleanup"),
            "evidence": _binding(bindings, "cmd_evidence"),
            "status": _binding(bindings, "cmd_status"),
            "desktop": _binding(bindings, "cmd_desktop"),
        },
        simulator_commands={
            "video-doctor": _binding(bindings, "cmd_simulator_video_doctor"),
            "video": _binding(bindings, "cmd_simulator_video"),
        },
        android_commands={
            "video-doctor": bindings.get("cmd_android_video_doctor"),
            "video": bindings.get("cmd_android_video"),
        },
        cloud_commands={
            "workflows": _binding(bindings, "cmd_cloud_workflows"),
            "defaults": _binding(bindings, "cmd_cloud_defaults"),
            "history": _binding(bindings, "cmd_cloud_history"),
            "compare": _binding(bindings, "cmd_cloud_compare"),
            "recommend": _binding(bindings, "cmd_cloud_recommend"),
            "run": _binding(bindings, "cmd_cloud_run"),
            "status": _binding(bindings, "cmd_cloud_status"),
        },
        cloud_namespace_commands={
            "doctor": _binding(bindings, "cmd_cloud_namespace_doctor"),
            "setup": _binding(bindings, "cmd_cloud_namespace_setup"),
        },
        print_help=print_help,
    )
