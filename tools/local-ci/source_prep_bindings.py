"""Bindings from the local_ci facade to desktop source-prep helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


def make_desktop_source_request(bindings: Mapping[str, Any], args: Any) -> dict:
    return _binding(bindings, "_source_prep").make_desktop_source_request(
        args,
        normalize_desktop_source_mode_fn=_binding(bindings, "normalize_desktop_source_mode"),
        current_branch_fn=_binding(bindings, "current_branch"),
        current_sha_fn=_binding(bindings, "current_sha"),
    )


def desktop_source_cache_key(bindings: Mapping[str, Any], source_request: dict) -> str:
    return _binding(bindings, "_source_prep").desktop_source_cache_key(source_request)


def desktop_source_root(bindings: Mapping[str, Any], target_name: str, source_request: dict) -> Path:
    return _binding(bindings, "_source_prep").desktop_source_root(
        target_name,
        source_request,
        state_dir_fn=_binding(bindings, "state_dir"),
    )


def command_path_rewrite_candidate(bindings: Mapping[str, Any], token: str) -> Path | None:
    return _binding(bindings, "_source_prep").command_path_rewrite_candidate(
        token,
        root=_binding(bindings, "ROOT"),
    )


def rewrite_launch_command_for_mapper(
    bindings: Mapping[str, Any],
    command: str | None,
    mapper,
    *,
    windows: bool = False,
) -> str | None:
    return _binding(bindings, "_source_prep").rewrite_launch_command_for_mapper(
        command,
        mapper,
        root=_binding(bindings, "ROOT"),
        windows=windows,
    )


def rewrite_launch_command_for_source_root(
    bindings: Mapping[str, Any],
    command: str | None,
    source_root: Path,
) -> str | None:
    return _binding(bindings, "_source_prep").rewrite_launch_command_for_source_root(
        command,
        source_root,
        root=_binding(bindings, "ROOT"),
    )


def rewrite_launch_command_for_posix_root(
    bindings: Mapping[str, Any],
    command: str | None,
    remote_root: str,
) -> str | None:
    return _binding(bindings, "_source_prep").rewrite_launch_command_for_posix_root(
        command,
        remote_root,
        root=_binding(bindings, "ROOT"),
    )


def rewrite_launch_command_for_windows_root(
    bindings: Mapping[str, Any],
    command: str | None,
    remote_root: str,
) -> str | None:
    return _binding(bindings, "_source_prep").rewrite_launch_command_for_windows_root(
        command,
        remote_root,
        root=_binding(bindings, "ROOT"),
        windows_path_join_fn=_binding(bindings, "windows_path_join"),
    )


def split_windows_prepare_commands(bindings: Mapping[str, Any], command: str) -> list[str]:
    return _binding(bindings, "_source_prep").split_windows_prepare_commands(command)


def validate_windows_prepare_commands(bindings: Mapping[str, Any], commands: list[str]) -> None:
    return _binding(bindings, "_source_prep").validate_windows_prepare_commands(commands)


def attach_desktop_source_to_manifest(
    bindings: Mapping[str, Any],
    manifest: dict,
    source_context: dict | None,
) -> None:
    return _binding(bindings, "_source_prep").attach_desktop_source_to_manifest(manifest, source_context)


def local_worktree_matches(bindings: Mapping[str, Any], path: Path, sha: str) -> bool:
    return _binding(bindings, "_source_prep").local_worktree_matches(
        path,
        sha,
        run_fn=_binding_attr(bindings, "subprocess", "run"),
    )


def reset_local_worktree(bindings: Mapping[str, Any], path: Path) -> None:
    return _binding(bindings, "_source_prep").reset_local_worktree(
        path,
        root=_binding(bindings, "ROOT"),
        run_fn=_binding_attr(bindings, "subprocess", "run"),
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
        run_fn=_binding_attr(bindings, "subprocess", "run"),
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
        run_fn=_binding_attr(bindings, "subprocess", "run"),
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
