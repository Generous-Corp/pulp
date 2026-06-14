#!/usr/bin/env python3
"""Facade-level filesystem, state path, git, and SSH helper integration tests."""

from __future__ import annotations

import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("local_ci.py")


def load_module():
    return load_module_from_path(
        MODULE_PATH,
        module_name="pulp_local_ci_filesystem_state_git_integration",
        add_module_dir=True,
    )


class FilesystemStateGitIntegrationTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_filesystem_helpers_cover_tails_atomic_writes_and_image_hashes(self) -> None:
        self.assertEqual(self.mod.tail_lines(self.root / "missing.log"), [])
        log = self.root / "job.log"
        log.write_text("one\ntwo\nthree\n")
        self.assertEqual(self.mod.tail_lines(log, limit=2), ["two\n", "three\n"])
        self.assertEqual(self.mod.trim_line(" short ", max_len=10), "short")
        self.assertEqual(self.mod.trim_line("abcdef", max_len=4), "…def")

        with mock.patch.dict(os.environ, {"PULP_LOCAL_CI_HOME": str(self.root / "state")}, clear=True):
            output = self.root / "nested" / "result.json"
            output.parent.mkdir()
            self.mod.atomic_write_text(output, "payload")
            self.assertEqual(output.read_text(), "payload")

        before = self.root / "before.bin"
        after = self.root / "after.bin"
        before.write_bytes(b"abc")
        after.write_bytes(b"abd")
        summary = self.mod.image_change_summary(before, after)
        self.assertTrue(summary["changed"])
        self.assertIn(summary["method"], {"file-hash", "pixel-bbox"})

    def test_state_paths_git_helpers_and_ssh_retry_edges(self) -> None:
        with mock.patch.object(self.mod.Path, "home", return_value=self.root / "home"):
            with mock.patch.object(self.mod.sys, "platform", "darwin"):
                with mock.patch.dict(os.environ, {}, clear=True):
                    self.assertEqual(
                        self.mod.state_dir(),
                        self.root / "home" / "Library" / "Application Support" / "Pulp" / "local-ci",
                    )

        script_dir = self.root / "script"
        state_dir = self.root / "state"
        shared_config = state_dir / "config.json"
        shared_config.parent.mkdir(parents=True)
        shared_config.write_text("{}\n")
        state_paths_mod = sys.modules["state_paths"]
        with mock.patch.object(state_paths_mod, "SCRIPT_DIR", script_dir):
            with mock.patch.dict(os.environ, {"PULP_LOCAL_CI_HOME": str(state_dir)}, clear=True):
                self.assertEqual(self.mod.config_path(), shared_config)
                shared_config.unlink()
                self.assertEqual(self.mod.config_path(), script_dir / "config.json")
                self.assertEqual(self.mod.worktree_config_path(), script_dir / "config.json")
                self.assertEqual(self.mod.shared_config_path(), state_dir / "config.json")
                self.assertEqual(self.mod.drain_lock_path(), state_dir / "drain.lock")
                self.assertEqual(self.mod.runner_info_path(), state_dir / "runner.json")

        transient = subprocess.CompletedProcess(["ssh"], 255, stdout="", stderr="Connection reset by peer")
        success = subprocess.CompletedProcess(["ssh"], 0, stdout="ok", stderr="")
        with mock.patch.object(self.mod.subprocess, "run", side_effect=[transient, success]) as run:
            with mock.patch.object(self.mod.time, "sleep") as sleep:
                result = self.mod.run_ssh_subprocess(
                    ["ssh", "host"],
                    input="payload",
                    timeout=5,
                    retries=3,
                    retry_delay_secs=0.25,
                )
        self.assertEqual(result.stdout, "ok")
        self.assertEqual(run.call_count, 2)
        sleep.assert_called_once_with(0.25)

        permanent = subprocess.CompletedProcess(["ssh"], 255, stdout="", stderr="permission denied")
        with mock.patch.object(self.mod.subprocess, "run", return_value=permanent) as run:
            with mock.patch.object(self.mod.time, "sleep") as sleep:
                self.assertIs(self.mod.run_ssh_subprocess(["ssh", "host"]), permanent)
        run.assert_called_once()
        sleep.assert_not_called()

        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["git"], 1, stdout="", stderr="fatal"),
        ):
            self.assertIsNone(self.mod.git_root_for(self.root))
        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["git"], 0, stdout=f"{self.root}\n", stderr=""),
        ):
            self.assertEqual(self.mod.git_root_for(self.root), self.root.resolve())
        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["git"], 0, stdout="feature/local-ci\n", stderr=""),
        ):
            self.assertEqual(self.mod.current_branch(), "feature/local-ci")
        sha = "f" * 40
        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["git"], 0, stdout=f"{sha}\n", stderr=""),
        ):
            self.assertEqual(self.mod.current_sha(), sha)
            self.assertEqual(self.mod.resolve_git_ref_sha("HEAD"), sha)
        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["git"], 1, stdout="", stderr="bad ref"),
        ):
            with self.assertRaisesRegex(ValueError, "bad ref"):
                self.mod.resolve_git_ref_sha("missing")


if __name__ == "__main__":
    unittest.main()
