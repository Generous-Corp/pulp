"""Branch worktree publishing for desktop automation reports."""

from __future__ import annotations

from pathlib import Path
import shutil
import tempfile
from typing import Callable

from reporting_publish_branch import desktop_branch_publish_metadata


def publish_report_to_branch(
    config: dict,
    report: dict,
    *,
    root: Path,
    run_git_fn: Callable[..., object],
    reset_local_worktree_fn: Callable[[Path], None],
    clear_directory_contents_fn: Callable[[Path], None],
    git_origin_http_url_fn: Callable[[Path], str | None],
) -> dict:
    branch = config["desktop_automation"]["publish_branch"]
    report_dir = Path(report["output_dir"]).expanduser()
    report_name = report_dir.name
    publish_root = Path(tempfile.mkdtemp(prefix="pulp-desktop-publish-"))
    worktree = publish_root / "worktree"
    branch_exists = bool(run_git_fn(["ls-remote", "--heads", "origin", branch], cwd=root, check=False).stdout.strip())
    try:
        if branch_exists:
            run_git_fn(["worktree", "add", "--detach", str(worktree), f"origin/{branch}"], cwd=root)
            run_git_fn(["checkout", "-B", branch, f"origin/{branch}"], cwd=worktree)
        else:
            run_git_fn(["worktree", "add", "--detach", str(worktree), "HEAD"], cwd=root)
            run_git_fn(["checkout", "--orphan", branch], cwd=worktree)
            run_git_fn(["rm", "-rf", "--ignore-unmatch", "."], cwd=worktree, check=False)
            clear_directory_contents_fn(worktree)
        dest_root = worktree / "desktop-automation"
        report_dest = dest_root / "reports" / report_name
        latest_dest = dest_root / "latest"
        shutil.rmtree(report_dest, ignore_errors=True)
        shutil.rmtree(latest_dest, ignore_errors=True)
        report_dest.parent.mkdir(parents=True, exist_ok=True)
        latest_dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(report_dir, report_dest)
        shutil.copytree(report_dir, latest_dest)
        run_git_fn(["add", "desktop-automation"], cwd=worktree)
        status = run_git_fn(["status", "--short"], cwd=worktree).stdout.strip()
        if status:
            run_git_fn(["commit", "-m", f"Publish desktop automation report {report_name}"], cwd=worktree)
            run_git_fn(["push", "origin", f"HEAD:{branch}"], cwd=worktree)
        remote_base = git_origin_http_url_fn(root)
        return desktop_branch_publish_metadata(report, branch=branch, report_name=report_name, remote_base=remote_base)
    finally:
        reset_local_worktree_fn(worktree)
        shutil.rmtree(publish_root, ignore_errors=True)


__all__ = ["publish_report_to_branch"]
