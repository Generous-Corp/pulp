"""Bindings from the local_ci facade to target preflight helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr
from binding_utils import print_binding as _print_binding


def ssh_probe(bindings: Mapping[str, Any], host: str, timeout: int = 5) -> Any:
    return _binding(bindings, "_target_preflight").ssh_probe(
        host,
        timeout,
        run_ssh_subprocess_fn=_binding(bindings, "run_ssh_subprocess"),
    )


def ssh_reachable(bindings: Mapping[str, Any], host: str, timeout: int = 5) -> bool:
    return _binding(bindings, "_target_preflight").ssh_reachable(
        host,
        timeout,
        ssh_probe_fn=_binding(bindings, "ssh_probe"),
    )


def ssh_failure_detail(bindings: Mapping[str, Any], host: str, timeout: int = 5) -> str:
    return _binding(bindings, "_target_preflight").ssh_failure_detail(
        host,
        timeout,
        ssh_probe_fn=_binding(bindings, "ssh_probe"),
    )


def ssh_command_result(bindings: Mapping[str, Any], host: str, remote_cmd: str, *, timeout: int = 30) -> Any:
    return _binding(bindings, "_target_preflight").ssh_command_result(
        host,
        remote_cmd,
        timeout=timeout,
        run_ssh_subprocess_fn=_binding(bindings, "run_ssh_subprocess"),
    )


def utmctl_vm_status(bindings: Mapping[str, Any], vm_name: str) -> str | None:
    return _binding(bindings, "_target_preflight").utmctl_vm_status(
        vm_name,
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def utmctl_start(bindings: Mapping[str, Any], vm_name: str) -> bool:
    return _binding(bindings, "_target_preflight").utmctl_start(
        vm_name,
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def ensure_host_reachable(bindings: Mapping[str, Any], target_name: str, target_cfg: dict, defaults: dict) -> str | None:
    return _binding(bindings, "_target_preflight").ensure_host_reachable(
        target_name,
        target_cfg,
        defaults,
        ssh_reachable_fn=_binding(bindings, "ssh_reachable"),
        utmctl_vm_status_fn=_binding(bindings, "utmctl_vm_status"),
        utmctl_start_fn=_binding(bindings, "utmctl_start"),
        time_fn=_binding_attr(bindings, "time", "time"),
        sleep_fn=_binding_attr(bindings, "time", "sleep"),
        print_fn=_print_binding(bindings),
    )


def config_source_name(bindings: Mapping[str, Any], path: Path) -> str:
    return _binding(bindings, "_target_preflight").config_source_name(
        path,
        environ=_binding(bindings, "os").environ,
        shared_config_path_fn=_binding(bindings, "shared_config_path"),
    )


def config_material_for_targets(bindings: Mapping[str, Any], config: dict, targets: list[str]) -> dict:
    return _binding(bindings, "_target_preflight").config_material_for_targets(config, targets)


def find_material_config_drift(bindings: Mapping[str, Any], targets: list[str]) -> list[str]:
    return _binding(bindings, "_target_preflight").find_material_config_drift(
        targets,
        shared_config_path_fn=_binding(bindings, "shared_config_path"),
        worktree_config_path_fn=_binding(bindings, "worktree_config_path"),
        config_material_for_targets_fn=_binding(bindings, "config_material_for_targets"),
    )


def preflight_target_host_state(bindings: Mapping[str, Any], target_name: str, target_cfg: dict, defaults: dict) -> dict:
    return _binding(bindings, "_target_preflight").preflight_target_host_state(
        target_name,
        target_cfg,
        defaults,
        ssh_reachable_fn=_binding(bindings, "ssh_reachable"),
    )


def build_submission_metadata(
    bindings: Mapping[str, Any],
    config: dict,
    branch: str,
    sha: str,
    targets: list[str],
    priority: str,
    validation: str,
    *,
    allow_root_mismatch: bool,
    allow_unreachable_targets: bool,
) -> dict:
    return _binding(bindings, "_target_preflight").build_submission_metadata(
        config,
        branch,
        sha,
        targets,
        priority,
        validation,
        allow_root_mismatch=allow_root_mismatch,
        allow_unreachable_targets=allow_unreachable_targets,
        root=_binding(bindings, "ROOT"),
        cwd_fn=Path.cwd,
        git_root_for_fn=_binding(bindings, "git_root_for"),
        config_path_fn=_binding(bindings, "config_path"),
        config_source_name_fn=_binding(bindings, "config_source_name"),
        preflight_target_host_state_fn=_binding(bindings, "preflight_target_host_state"),
        find_material_config_drift_fn=_binding(bindings, "find_material_config_drift"),
        normalize_provenance_fn=_binding(bindings, "normalize_provenance"),
        environ=_binding(bindings, "os").environ,
    )


def print_submission_metadata(bindings: Mapping[str, Any], metadata: dict) -> None:
    return _binding(bindings, "_target_preflight").print_submission_metadata(
        metadata,
        short_sha_fn=_binding(bindings, "short_sha"),
        provenance_summary_fn=_binding(bindings, "provenance_summary"),
        print_fn=_print_binding(bindings),
    )
