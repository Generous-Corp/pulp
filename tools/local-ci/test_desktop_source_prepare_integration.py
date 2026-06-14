#!/usr/bin/env python3
"""Facade-level desktop exact-SHA source preparation integration tests."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("local_ci.py")


def load_module():
    return load_module_from_path(
        MODULE_PATH,
        module_name="pulp_local_ci_desktop_source_prepare_integration",
        add_module_dir=True,
    )


class DesktopSourcePrepareIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_exact_sha_prepare_and_remote_artifact_helpers_cover_edges(self) -> None:
        bundle_dir = self.root / "bundle"
        bundle_dir.mkdir()
        source_request = {
            "mode": "exact-sha",
            "branch": "feature/source",
            "sha": "abc123",
            "prepare_command": "echo prepare",
            "prepare_timeout_secs": 10,
        }

        with mock.patch.object(self.mod, "desktop_source_root", return_value=self.root / "prepared"), \
             mock.patch.object(self.mod, "_local_worktree_matches", return_value=False), \
             mock.patch.object(self.mod, "_reset_local_worktree") as reset_worktree, \
             mock.patch.object(self.mod, "run_logged_command", return_value={"timed_out": False, "returncode": 0}), \
             mock.patch.object(self.mod, "rewrite_launch_command_for_source_root", return_value="prepared-command"), \
             mock.patch.object(self.mod.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, stdout="", stderr="")) as run:
            prepared = self.mod.prepare_macos_exact_sha_source(bundle_dir, "mac", "./tool --flag", source_request)
        self.assertEqual(prepared["prepared_state"], "clean")
        self.assertEqual(prepared["launch_command"], "prepared-command")
        self.assertEqual(prepared["launch_cwd"], str(self.root / "prepared"))
        self.assertEqual(prepared["prepare_log"], None)
        self.assertEqual(run.call_args.args[0][:3], ["git", "worktree", "add"])
        self.assertEqual(run.call_args.kwargs["cwd"], self.mod.ROOT)
        reset_worktree.assert_called_once_with(self.root / "prepared")

        with mock.patch.object(self.mod, "desktop_source_root", return_value=self.root / "prepared"), \
             mock.patch.object(self.mod, "_local_worktree_matches", return_value=True), \
             mock.patch.object(self.mod, "_reset_local_worktree") as reset_worktree, \
             mock.patch.object(self.mod, "run_logged_command") as logged_command, \
             mock.patch.object(self.mod, "rewrite_launch_command_for_source_root", return_value="reused-command"):
            reused = self.mod.prepare_macos_exact_sha_source(bundle_dir, "mac", "./tool", source_request)
        self.assertEqual(reused["prepared_state"], "reused")
        self.assertEqual(reused["launch_command"], "reused-command")
        self.assertEqual(reused["prepared_root"], str(self.root / "prepared"))
        self.assertEqual(reused["prepare_log"], None)
        reset_worktree.assert_not_called()
        logged_command.assert_not_called()

        with mock.patch.object(self.mod, "desktop_source_root", return_value=self.root / "prepared"), \
             mock.patch.object(self.mod, "_local_worktree_matches", return_value=False), \
             mock.patch.object(self.mod, "_reset_local_worktree"), \
             mock.patch.object(self.mod.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, stdout="", stderr="")), \
             mock.patch.object(self.mod, "run_logged_command", return_value={"timed_out": True, "returncode": 0}):
            with self.assertRaisesRegex(RuntimeError, "Timed out preparing"):
                self.mod.prepare_macos_exact_sha_source(bundle_dir, "mac", "./tool", source_request)

        with mock.patch.object(self.mod, "sync_job_bundle_to_ssh_host", return_value=("source.bundle", "refs/bundle")), \
             mock.patch.object(self.mod, "git_origin_clone_url", return_value="https://example/pulp.git"), \
             mock.patch.object(self.mod, "desktop_source_cache_key", return_value="cache-key"), \
             mock.patch.object(self.mod, "rewrite_launch_command_for_posix_root", return_value="remote-command"), \
             mock.patch.object(self.mod, "fetch_ssh_artifact", return_value=True) as fetch_artifact, \
             mock.patch.object(self.mod.subprocess, "run", side_effect=[
                 subprocess.CompletedProcess([], 0, stdout="/home/dev\n", stderr=""),
                 subprocess.CompletedProcess([], 0, stdout="__PULP_PREPARED__:reused\n", stderr=""),
             ]) as run:
            linux = self.mod.prepare_linux_exact_sha_source(bundle_dir, "ubuntu", "host", "./tool", source_request)
        self.assertEqual(linux["prepared_state"], "reused")
        self.assertEqual(linux["prepared_root"], "/home/dev/.local/state/pulp/desktop-source/ubuntu/cache-key")
        self.assertEqual(linux["prepared_root_display"], "~/.local/state/pulp/desktop-source/ubuntu/cache-key")
        self.assertEqual(linux["launch_command"], "remote-command")
        self.assertEqual(linux["prepare_log"], None)
        self.assertEqual(run.call_count, 2)
        self.assertEqual(run.call_args_list[0].args[0][:2], ["ssh", "host"])
        self.assertIn("PULP_REQUIRE_PREPARE_STAMP", run.call_args_list[1].args[0][-1])
        fetch_artifact.assert_called_once()

        with mock.patch.object(self.mod, "sync_job_bundle_to_ssh_host", return_value=("source.bundle", "refs/bundle")), \
             mock.patch.object(self.mod.subprocess, "run", return_value=subprocess.CompletedProcess([], 1, stdout="", stderr="no home")):
            with self.assertRaisesRegex(RuntimeError, "no home"):
                self.mod.prepare_linux_exact_sha_source(bundle_dir, "ubuntu", "host", "./tool", source_request)

        with mock.patch.object(self.mod, "sync_job_bundle_to_ssh_host", return_value=("source.bundle", "refs/bundle")), \
             mock.patch.object(self.mod, "git_origin_clone_url", return_value=""), \
             mock.patch.object(self.mod, "desktop_source_cache_key", return_value="cache-key"), \
             mock.patch.object(self.mod, "split_windows_prepare_commands", return_value=["echo prepare"]), \
             mock.patch.object(self.mod, "validate_windows_prepare_commands") as validate_commands, \
             mock.patch.object(self.mod, "rewrite_launch_command_for_windows_root", return_value="win-command"), \
             mock.patch.object(self.mod, "windows_ssh_fetch_file", return_value=True) as fetch_file, \
             mock.patch.object(self.mod, "run_windows_ssh_powershell", return_value=subprocess.CompletedProcess([], 0, stdout="__PULP_PREPARED__:clean\n", stderr="")) as run_ps:
            windows = self.mod.prepare_windows_exact_sha_source(bundle_dir, "windows", "win", r".\tool.exe", source_request)
        self.assertEqual(windows["prepared_state"], "clean")
        self.assertEqual(windows["prepared_root"], r"%LOCALAPPDATA%\Pulp\desktop-source\windows\cache-key")
        self.assertEqual(windows["launch_command"], "win-command")
        self.assertEqual(windows["prepare_log"], None)
        self.assertIn("PULP_REQUIRE_PREPARE_STAMP", run_ps.call_args.args[1])
        self.assertIn("@echo off", run_ps.call_args.args[1])
        self.assertEqual(run_ps.call_args.args[0], "win")
        validate_commands.assert_called_once_with(["echo prepare"])
        fetch_file.assert_called_once()

        copied = self.root / "artifacts" / "remote.txt"
        with mock.patch.object(
            self.mod.subprocess,
            "run",
            side_effect=lambda *_args, **_kwargs: (copied.write_text("payload"), subprocess.CompletedProcess([], 0, stdout="", stderr=""))[1],
        ):
            self.assertTrue(self.mod.fetch_ssh_artifact("host", "/tmp/remote.txt", copied))
        self.assertEqual(copied.read_text(), "payload")
        self.assertTrue(copied.parent.is_dir())

        with mock.patch.object(self.mod.subprocess, "run", return_value=subprocess.CompletedProcess([], 1, stdout="", stderr="missing")):
            self.assertFalse(self.mod.fetch_ssh_artifact("host", "/tmp/missing.txt", self.root / "optional.txt", optional=True))
            self.assertFalse((self.root / "optional.txt").exists())
            with self.assertRaisesRegex(RuntimeError, "missing"):
                self.mod.fetch_ssh_artifact("host", "/tmp/missing.txt", self.root / "required.txt")


if __name__ == "__main__":
    unittest.main()
