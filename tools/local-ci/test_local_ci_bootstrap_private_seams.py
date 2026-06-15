#!/usr/bin/env python3
"""Tests for local_ci bootstrap private compatibility seams."""

from __future__ import annotations

import unittest

from module_test_utils import load_local_ci_module


private_seams = load_local_ci_module(
    "local_ci_bootstrap_private_seams.py",
    module_name="local_ci_bootstrap_private_seams",
    add_module_dir=True,
)


class LocalCiBootstrapPrivateSeamsTests(unittest.TestCase):
    def test_install_desktop_private_seams_aliases_public_helpers(self) -> None:
        names = (
            "desktop_check",
            "check_writable_dir",
            "clear_directory_contents",
            "copy_directory_contents",
            "run_git",
            "command_path_rewrite_candidate",
            "rewrite_launch_command_for_mapper",
            "local_worktree_matches",
            "reset_local_worktree",
        )
        bindings = {name: object() for name in names}

        private_seams.install_desktop_private_seams(bindings)

        self.assertIs(bindings["_desktop_check"], bindings["desktop_check"])
        self.assertIs(bindings["_check_writable_dir"], bindings["check_writable_dir"])
        self.assertIs(bindings["_clear_directory_contents"], bindings["clear_directory_contents"])
        self.assertIs(bindings["_copy_directory_contents"], bindings["copy_directory_contents"])
        self.assertIs(bindings["_run_git"], bindings["run_git"])
        self.assertIs(
            bindings["_command_path_rewrite_candidate"],
            bindings["command_path_rewrite_candidate"],
        )
        self.assertIs(
            bindings["_rewrite_launch_command_for_mapper"],
            bindings["rewrite_launch_command_for_mapper"],
        )
        self.assertIs(bindings["_local_worktree_matches"], bindings["local_worktree_matches"])
        self.assertIs(bindings["_reset_local_worktree"], bindings["reset_local_worktree"])

    def test_install_execution_private_seams_aliases_build_target_tasks(self) -> None:
        bindings = {"build_target_tasks": object()}

        private_seams.install_execution_private_seams(bindings)

        self.assertIs(bindings["_build_target_tasks"], bindings["build_target_tasks"])


if __name__ == "__main__":
    unittest.main()
