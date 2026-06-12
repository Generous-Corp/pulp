"""Exact-SHA desktop source materialization helpers for local CI."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path
import shlex
import shutil
import subprocess
import uuid

from source_prep_exact_scripts import (
    build_linux_exact_sha_prepare_command,
    build_windows_exact_sha_prepare_script,
)


def local_worktree_matches(path: Path, sha: str, *, run_fn: Callable[..., subprocess.CompletedProcess]) -> bool:
    if not (path / ".git").exists():
        return False
    result = run_fn(
        ["git", "-C", str(path), "rev-parse", "HEAD"],
        capture_output=True,
        text=True,
        check=False,
    )
    return result.returncode == 0 and result.stdout.strip() == sha


def reset_local_worktree(
    path: Path,
    *,
    root: Path,
    run_fn: Callable[..., subprocess.CompletedProcess],
    rmtree_fn: Callable[..., None] = shutil.rmtree,
) -> None:
    run_fn(
        ["git", "worktree", "remove", "--force", str(path)],
        cwd=root,
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    rmtree_fn(path, ignore_errors=True)
    run_fn(
        ["git", "worktree", "prune"],
        cwd=root,
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )


def prepare_macos_exact_sha_source(
    bundle_dir: Path,
    target_name: str,
    command: str,
    source_request: dict,
    *,
    root: Path,
    desktop_source_root_fn: Callable[[str, dict], Path],
    local_worktree_matches_fn: Callable[[Path, str], bool],
    reset_local_worktree_fn: Callable[[Path], None],
    run_fn: Callable[..., subprocess.CompletedProcess],
    run_logged_command_fn: Callable,
    tail_lines_fn: Callable[..., list[str]],
    rewrite_launch_command_for_source_root_fn: Callable[[str | None, Path], str | None],
) -> dict:
    prepared_root = desktop_source_root_fn(target_name, source_request)
    prepare_log = bundle_dir / "prepare.log"
    reused = local_worktree_matches_fn(prepared_root, source_request["sha"])
    if not reused:
        reset_local_worktree_fn(prepared_root)
        prepared_root.parent.mkdir(parents=True, exist_ok=True)
        run_fn(
            ["git", "worktree", "add", "--detach", str(prepared_root), source_request["sha"]],
            cwd=root,
            check=True,
            capture_output=True,
            text=True,
        )
    if source_request.get("prepare_command") and not reused:
        run = run_logged_command_fn(
            ["bash", "-lc", source_request["prepare_command"]],
            cwd=prepared_root,
            timeout=int(source_request.get("prepare_timeout_secs", 900.0)),
            log_path=prepare_log,
        )
        if run["timed_out"]:
            raise RuntimeError(
                f"Timed out preparing desktop source for {target_name} after {source_request['prepare_timeout_secs']}s."
            )
        if run["returncode"] != 0:
            detail = tail_lines_fn(prepare_log, limit=40)
            raise RuntimeError("Desktop source prepare failed:\n" + "".join(detail).strip())
    return {
        **source_request,
        "prepared_root": str(prepared_root),
        "launch_cwd": str(prepared_root),
        "launch_command": rewrite_launch_command_for_source_root_fn(command, prepared_root),
        "prepare_log": str(prepare_log) if prepare_log.exists() else None,
        "prepared_state": "reused" if reused else "clean",
    }


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
