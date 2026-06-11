"""Bindings from the local_ci facade to desktop source-prep helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any


def _binding(bindings: Mapping[str, Any], name: str) -> Any:
    return bindings[name]


def local_worktree_matches(bindings: Mapping[str, Any], path: Path, sha: str) -> bool:
    return _binding(bindings, "_source_prep").local_worktree_matches(
        path,
        sha,
        run_fn=_binding(bindings, "subprocess").run,
    )


def reset_local_worktree(bindings: Mapping[str, Any], path: Path) -> None:
    return _binding(bindings, "_source_prep").reset_local_worktree(
        path,
        root=_binding(bindings, "ROOT"),
        run_fn=_binding(bindings, "subprocess").run,
    )


def prepare_macos_exact_sha_source(
    bindings: Mapping[str, Any],
    bundle_dir: Path,
    target_name: str,
    command: str,
    source_request: dict,
) -> dict:
    return _binding(bindings, "_source_prep").prepare_macos_exact_sha_source(
        bundle_dir,
        target_name,
        command,
        source_request,
        root=_binding(bindings, "ROOT"),
        desktop_source_root_fn=_binding(bindings, "desktop_source_root"),
        local_worktree_matches_fn=_binding(bindings, "_local_worktree_matches"),
        reset_local_worktree_fn=_binding(bindings, "_reset_local_worktree"),
        run_fn=_binding(bindings, "subprocess").run,
        run_logged_command_fn=_binding(bindings, "run_logged_command"),
        tail_lines_fn=_binding(bindings, "tail_lines"),
        rewrite_launch_command_for_source_root_fn=_binding(bindings, "rewrite_launch_command_for_source_root"),
    )


def prepare_linux_exact_sha_source(
    bindings: Mapping[str, Any],
    bundle_dir: Path,
    target_name: str,
    host: str,
    command: str,
    source_request: dict,
) -> dict:
    return _binding(bindings, "_source_prep").prepare_linux_exact_sha_source(
        bundle_dir,
        target_name,
        host,
        command,
        source_request,
        sync_job_bundle_to_ssh_host_fn=_binding(bindings, "sync_job_bundle_to_ssh_host"),
        git_origin_clone_url_fn=_binding(bindings, "git_origin_clone_url"),
        desktop_source_cache_key_fn=_binding(bindings, "desktop_source_cache_key"),
        root=_binding(bindings, "ROOT"),
        run_fn=_binding(bindings, "subprocess").run,
        fetch_ssh_artifact_fn=_binding(bindings, "fetch_ssh_artifact"),
        rewrite_launch_command_for_posix_root_fn=_binding(bindings, "rewrite_launch_command_for_posix_root"),
    )


def prepare_windows_exact_sha_source(
    bindings: Mapping[str, Any],
    bundle_dir: Path,
    target_name: str,
    host: str,
    command: str,
    source_request: dict,
) -> dict:
    return _binding(bindings, "_source_prep").prepare_windows_exact_sha_source(
        bundle_dir,
        target_name,
        host,
        command,
        source_request,
        sync_job_bundle_to_ssh_host_fn=_binding(bindings, "sync_job_bundle_to_ssh_host"),
        git_origin_clone_url_fn=_binding(bindings, "git_origin_clone_url"),
        desktop_source_cache_key_fn=_binding(bindings, "desktop_source_cache_key"),
        root=_binding(bindings, "ROOT"),
        ps_literal_fn=_binding(bindings, "ps_literal"),
        windows_contract_expand_expression_fn=_binding(bindings, "windows_contract_expand_expression"),
        split_windows_prepare_commands_fn=_binding(bindings, "split_windows_prepare_commands"),
        validate_windows_prepare_commands_fn=_binding(bindings, "validate_windows_prepare_commands"),
        run_windows_ssh_powershell_fn=_binding(bindings, "run_windows_ssh_powershell"),
        windows_ssh_fetch_file_fn=_binding(bindings, "windows_ssh_fetch_file"),
        rewrite_launch_command_for_windows_root_fn=_binding(bindings, "rewrite_launch_command_for_windows_root"),
    )
