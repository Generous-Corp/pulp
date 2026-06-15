"""macOS exact-SHA desktop source materialization."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path
import subprocess


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
