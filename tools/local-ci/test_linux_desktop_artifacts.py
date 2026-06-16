#!/usr/bin/env python3
"""No-network tests for Linux desktop SSH artifact helpers."""

from __future__ import annotations

from pathlib import Path
import subprocess
import tempfile
import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("linux_desktop_artifacts.py")


class LinuxDesktopArtifactsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_fetch_ssh_artifact_creates_parent_and_handles_optional_failures(self) -> None:
        copied = self.root / "artifacts" / "remote.txt"

        def successful_copy(_command, **_kwargs):
            copied.write_text("payload")
            return subprocess.CompletedProcess([], 0, stdout="", stderr="")

        self.assertTrue(self.mod.fetch_ssh_artifact("host", "/tmp/remote.txt", copied, run_fn=successful_copy))
        self.assertEqual(copied.read_text(), "payload")

        failed = self.root / "artifacts" / "missing.txt"
        failure = lambda *_args, **_kwargs: subprocess.CompletedProcess([], 1, stdout="", stderr="missing")
        self.assertFalse(
            self.mod.fetch_ssh_artifact("host", "/tmp/missing.txt", failed, optional=True, run_fn=failure)
        )
        with self.assertRaisesRegex(RuntimeError, "missing"):
            self.mod.fetch_ssh_artifact("host", "/tmp/missing.txt", failed, run_fn=failure)

    def test_cleanup_remote_ssh_dir_swallows_cleanup_errors(self) -> None:
        calls = []

        def cleanup(host, command, *, timeout):
            calls.append((host, command, timeout))
            raise RuntimeError("ssh unavailable")

        self.mod.cleanup_remote_ssh_dir("host", '"$HOME/bundle"', ssh_command_result_fn=cleanup)
        self.assertEqual(calls, [("host", 'rm -rf "$HOME/bundle"', 20)])


if __name__ == "__main__":
    unittest.main()
