"""Private compatibility seam installation for the local_ci.py facade."""

from __future__ import annotations

from typing import Any


def install_desktop_private_seams(bindings: dict[str, Any]) -> None:
    bindings["_desktop_check"] = bindings["desktop_check"]
    bindings["_check_writable_dir"] = bindings["check_writable_dir"]
    bindings["_clear_directory_contents"] = bindings["clear_directory_contents"]
    bindings["_copy_directory_contents"] = bindings["copy_directory_contents"]
    bindings["_run_git"] = bindings["run_git"]
    bindings["_command_path_rewrite_candidate"] = bindings["command_path_rewrite_candidate"]
    bindings["_rewrite_launch_command_for_mapper"] = bindings["rewrite_launch_command_for_mapper"]
    bindings["_local_worktree_matches"] = bindings["local_worktree_matches"]
    bindings["_reset_local_worktree"] = bindings["reset_local_worktree"]


def install_execution_private_seams(bindings: dict[str, Any]) -> None:
    bindings["_build_target_tasks"] = bindings["build_target_tasks"]
