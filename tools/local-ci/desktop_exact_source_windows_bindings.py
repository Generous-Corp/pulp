"""Bindings from the local_ci facade to Windows remote exact-source helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import install_local_helpers


DESKTOP_EXACT_SOURCE_WINDOWS_EXPORTS = ("prepare_windows_exact_sha_source",)


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


def install_desktop_exact_source_windows_helpers(
    bindings: dict[str, Any],
    names: tuple[str, ...] = DESKTOP_EXACT_SOURCE_WINDOWS_EXPORTS,
) -> None:
    install_local_helpers(bindings, globals(), names)
