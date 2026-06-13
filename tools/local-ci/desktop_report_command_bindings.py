"""Bindings from the local_ci facade to desktop report command helpers."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

from binding_utils import install_local_helpers
from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


DESKTOP_REPORT_COMMAND_EXPORTS = (
    "cmd_desktop_recent",
    "cmd_desktop_proof",
    "cmd_desktop_publish",
    "cmd_desktop_cleanup",
)


def cmd_desktop_recent(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_recent(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        desktop_run_manifests_fn=_binding(bindings, "desktop_run_manifests"),
        desktop_run_summary_fn=_binding(bindings, "desktop_run_summary"),
        desktop_recent_lines_fn=_binding_attr(bindings, "_desktop_cli", "desktop_recent_lines"),
        short_sha_fn=_binding(bindings, "short_sha"),
    )


def cmd_desktop_proof(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_proof(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        desktop_proof_summaries_fn=_binding(bindings, "desktop_proof_summaries"),
        desktop_proof_empty_line_fn=_binding_attr(bindings, "_desktop_cli", "desktop_proof_empty_line"),
        desktop_proof_lines_fn=_binding_attr(bindings, "_desktop_cli", "desktop_proof_lines"),
        short_sha_fn=_binding(bindings, "short_sha"),
    )


def cmd_desktop_publish(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_publish(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        desktop_run_manifests_fn=_binding(bindings, "desktop_run_manifests"),
        stage_desktop_publish_report_fn=_binding(bindings, "stage_desktop_publish_report"),
        desktop_publish_lines_fn=_binding_attr(bindings, "_desktop_cli", "desktop_publish_lines"),
    )


def cmd_desktop_cleanup(bindings: Mapping[str, Any], args: Any) -> int:
    return _binding(bindings, "_desktop_commands_cli").cmd_desktop_cleanup(
        args,
        load_config_fn=_binding(bindings, "load_config"),
        prune_desktop_run_manifests_fn=_binding(bindings, "prune_desktop_run_manifests"),
        write_desktop_run_rollups_fn=_binding(bindings, "write_desktop_run_rollups"),
        desktop_cleanup_empty_line_fn=_binding_attr(bindings, "_desktop_cli", "desktop_cleanup_empty_line"),
        desktop_cleanup_lines_fn=_binding_attr(bindings, "_desktop_cli", "desktop_cleanup_lines"),
    )


def install_desktop_report_command_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_REPORT_COMMAND_EXPORTS,
) -> None:
    known_names = set(DESKTOP_REPORT_COMMAND_EXPORTS)
    command_names = tuple(name for name in names if name in known_names)
    unknown_names = tuple(name for name in names if name not in known_names)

    install_local_helpers(bindings, globals(), command_names)
    if unknown_names:
        install_local_helpers(bindings, globals(), unknown_names)
