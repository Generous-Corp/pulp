"""Bindings from the local_ci facade to Linux remote exact-source helpers."""

from __future__ import annotations

from collections.abc import Mapping
from pathlib import Path
from typing import Any

from binding_utils import binding as _binding
from binding_utils import binding_attr as _binding_attr


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
