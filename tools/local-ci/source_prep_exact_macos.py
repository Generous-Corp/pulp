"""macOS exact-SHA desktop source materialization."""

from __future__ import annotations

from collections.abc import Callable
from pathlib import Path
import shutil
import subprocess


def link_local_skia_build_for_prepared_source(root: Path, prepared_root: Path) -> str | None:
    """Symlink the live checkout's prebuilt Skia into a prepared exact-SHA root.

    Exact-SHA desktop sources are fresh ``git worktree`` checkouts without the
    machine-local ``external/skia-build/`` static libraries, so a GPU/Skia build
    (needed to record the design-tool / GPU demo) would otherwise fail. When the
    live checkout has a usable Skia build, link it in; return the linked path or
    ``None`` when no local Skia build is available.
    """
    local_skia = root / "external" / "skia-build"
    skia_candidates = (
        local_skia / "build" / "mac-gpu" / "lib" / "Release" / "libskia.a",
        local_skia / "mac-gpu" / "lib" / "Release" / "libskia.a",
        local_skia / "mac" / "lib" / "libskia.a",
        local_skia / "lib" / "libskia.a",
        local_skia / "libskia.a",
    )
    if not any(candidate.is_file() for candidate in skia_candidates):
        return None
    prepared_skia = prepared_root / "external" / "skia-build"
    if any((prepared_skia / candidate.relative_to(local_skia)).is_file() for candidate in skia_candidates):
        return str(prepared_skia)
    if prepared_skia.is_symlink():
        prepared_skia.unlink()
    elif prepared_skia.exists():
        shutil.rmtree(prepared_skia)
    prepared_skia.parent.mkdir(parents=True, exist_ok=True)
    prepared_skia.symlink_to(local_skia, target_is_directory=True)
    return str(prepared_skia)


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
    link_local_skia_build_for_prepared_source(root, prepared_root)
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
