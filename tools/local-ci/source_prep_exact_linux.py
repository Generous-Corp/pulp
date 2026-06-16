"""Linux exact-SHA desktop source materialization."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path
import shlex
import subprocess
import uuid

from source_prep_exact_scripts import build_linux_exact_sha_prepare_command


def prepare_linux_exact_sha_source(
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
    run_fn: Callable[..., subprocess.CompletedProcess],
    fetch_ssh_artifact_fn: Callable[..., bool],
    rewrite_launch_command_for_posix_root_fn: Callable[[str | None, str], str | None],
) -> dict:
    prepare_log = bundle_dir / "prepare.log"
    source_job = {"id": uuid.uuid4().hex[:12], "sha": source_request["sha"]}
    bundle_name, bundle_ref = sync_job_bundle_to_ssh_host_fn(host, source_job)
    remote_url = git_origin_clone_url_fn(root) or ""
    home_run = run_fn(
        ["ssh", host, "bash", "-lc", shlex.quote('printf %s "$HOME"')],
        capture_output=True,
        text=True,
        timeout=30,
    )
    if home_run.returncode != 0 or not home_run.stdout.strip():
        detail = home_run.stderr.strip() or home_run.stdout.strip() or "could not resolve remote home directory"
        raise RuntimeError(f"Linux exact-SHA prepare failed: {detail}")
    remote_home = home_run.stdout.strip()
    cache_key = desktop_source_cache_key_fn(source_request)
    prepared_root = f"{remote_home}/.local/state/pulp/desktop-source/{target_name}/{cache_key}"
    prepared_root_display = f"~/.local/state/pulp/desktop-source/{target_name}/{cache_key}"
    remote_prepare_log = prepared_root + "/prepare.log"
    prepare_stamp = prepared_root + "/.pulp-prepare-ok"
    prepare_cmd = build_linux_exact_sha_prepare_command(
        bundle_name=bundle_name,
        bundle_ref=bundle_ref,
        prepared_root=prepared_root,
        prepare_stamp=prepare_stamp,
        sha=source_request["sha"],
        remote_url=remote_url,
        prepare_command=source_request.get("prepare_command"),
        remote_prepare_log=remote_prepare_log,
    )
    run = run_fn(
        ["ssh", host, "bash", "-lc", shlex.quote(prepare_cmd)],
        capture_output=True,
        text=True,
        timeout=max(60, int(source_request.get("prepare_timeout_secs", 900.0) + 30)),
    )
    if source_request.get("prepare_command"):
        fetch_ssh_artifact_fn(host, remote_prepare_log, prepare_log, optional=True, timeout=60)
    if run.returncode != 0:
        detail = run.stderr.strip() or run.stdout.strip() or f"remote command exited {run.returncode}"
        raise RuntimeError(f"Linux exact-SHA prepare failed: {detail}")
    return {
        **source_request,
        "prepared_root": prepared_root,
        "prepared_root_display": prepared_root_display,
        "launch_cwd": prepared_root,
        "launch_cwd_display": prepared_root_display,
        "launch_command": rewrite_launch_command_for_posix_root_fn(command, prepared_root),
        "prepare_log": str(prepare_log) if prepare_log.exists() else None,
        "prepared_state": "reused" if "__PULP_PREPARED__:reused" in run.stdout else "clean",
    }
