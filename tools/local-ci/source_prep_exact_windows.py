"""Windows exact-SHA desktop source materialization."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path
import subprocess
import uuid

from source_prep_exact_scripts import build_windows_exact_sha_prepare_script


def prepare_windows_exact_sha_source(
    bundle_dir: Path,
    target_name: str,
    host: str,
    command: str,
    source_request: dict,
    *,
    sync_job_bundle_to_ssh_host_fn: Callable[[str, dict], tuple[str, str]],
    git_origin_clone_url_fn: Callable[[Path], str | None],
    desktop_source_cache_key_fn: Callable[[dict], str],
    root: Path,
    ps_literal_fn: Callable[[str], str],
    windows_contract_expand_expression_fn: Callable[[str], str],
    split_windows_prepare_commands_fn: Callable[[str], list[str]],
    validate_windows_prepare_commands_fn: Callable[[list[str]], None],
    run_windows_ssh_powershell_fn: Callable[..., subprocess.CompletedProcess],
    windows_ssh_fetch_file_fn: Callable[..., bool],
    rewrite_launch_command_for_windows_root_fn: Callable[[str | None, str], str | None],
) -> dict:
    prepare_log = bundle_dir / "prepare.log"
    source_job = {"id": uuid.uuid4().hex[:12], "sha": source_request["sha"]}
    bundle_name, bundle_ref = sync_job_bundle_to_ssh_host_fn(host, source_job)
    remote_url = git_origin_clone_url_fn(root) or ""
    cache_key = desktop_source_cache_key_fn(source_request)
    prepared_root = rf"%LOCALAPPDATA%\Pulp\desktop-source\{target_name}\{cache_key}"
    remote_prepare_log = prepared_root + r"\prepare.log"
    prepare_stamp = prepared_root + r"\.pulp-prepare-ok"
    prepare_script_path = prepared_root + r"\.pulp-prepare.cmd"
    prepare_script = build_windows_exact_sha_prepare_script(
        bundle_name=bundle_name,
        bundle_ref=bundle_ref,
        prepared_root=prepared_root,
        remote_prepare_log=remote_prepare_log,
        prepare_stamp=prepare_stamp,
        prepare_script_path=prepare_script_path,
        sha=source_request["sha"],
        remote_url=remote_url,
        prepare_command=source_request.get("prepare_command"),
        ps_literal_fn=ps_literal_fn,
        windows_contract_expand_expression_fn=windows_contract_expand_expression_fn,
        split_windows_prepare_commands_fn=split_windows_prepare_commands_fn,
        validate_windows_prepare_commands_fn=validate_windows_prepare_commands_fn,
    )
    run = run_windows_ssh_powershell_fn(
        host,
        prepare_script,
        timeout=max(60, int(source_request.get("prepare_timeout_secs", 900.0) + 30)),
    )
    if source_request.get("prepare_command"):
        windows_ssh_fetch_file_fn(host, remote_prepare_log, prepare_log, optional=True, timeout=60)
    if run.returncode != 0:
        detail = run.stderr.strip() or run.stdout.strip() or f"remote command exited {run.returncode}"
        raise RuntimeError(f"Windows exact-SHA prepare failed: {detail}")
    return {
        **source_request,
        "prepared_root": prepared_root,
        "launch_cwd": prepared_root,
        "launch_command": rewrite_launch_command_for_windows_root_fn(command, prepared_root),
        "prepare_log": str(prepare_log) if prepare_log.exists() else None,
        "prepared_state": "reused" if "__PULP_PREPARED__:reused" in run.stdout else "clean",
    }
