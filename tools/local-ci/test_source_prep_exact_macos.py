#!/usr/bin/env python3
"""No-network tests for macOS exact-SHA source materialization."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("source_prep_exact_macos.py", add_module_dir=True)


class SourcePrepExactMacOSTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)
        self.repo = self.root / "repo"
        self.repo.mkdir()

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def request(self, **overrides) -> dict:
        request = {
            "mode": "exact-sha",
            "branch": "feature/source",
            "sha": "a" * 40,
            "prepare_command": None,
            "prepare_timeout_secs": 120.0,
        }
        request.update(overrides)
        return request

    def test_prepare_macos_exact_sha_source_uses_clean_and_reused_paths(self) -> None:
        bundle_dir = self.root / "bundle-mac"
        bundle_dir.mkdir()
        prepared_root = self.root / "prepared-mac"
        source_request = self.request(prepare_command="echo prepare")
        run_calls = []

        def fake_run(command, **kwargs):
            run_calls.append((command, kwargs))
            return subprocess.CompletedProcess(command, 0, stdout="", stderr="")

        def fake_logged_command(command, **kwargs):
            kwargs["log_path"].write_text("prepared\n")
            return {"timed_out": False, "returncode": 0}

        clean = self.mod.prepare_macos_exact_sha_source(
            bundle_dir,
            "mac",
            "./tool --flag",
            source_request,
            root=self.repo,
            desktop_source_root_fn=lambda _target, _request: prepared_root,
            local_worktree_matches_fn=lambda _path, _sha: False,
            reset_local_worktree_fn=lambda path: run_calls.append((["reset", str(path)], {})),
            run_fn=fake_run,
            run_logged_command_fn=fake_logged_command,
            tail_lines_fn=lambda _path, limit=40: ["tail"],
            rewrite_launch_command_for_source_root_fn=lambda command, root: f"{root}:{command}",
        )

        self.assertEqual(clean["prepared_state"], "clean")
        self.assertEqual(clean["launch_command"], f"{prepared_root}:./tool --flag")
        self.assertEqual(clean["prepare_log"], str(bundle_dir / "prepare.log"))
        self.assertEqual(run_calls[0][0], ["reset", str(prepared_root)])
        self.assertEqual(run_calls[1][0][:3], ["git", "worktree", "add"])

        logged = []
        reused = self.mod.prepare_macos_exact_sha_source(
            bundle_dir,
            "mac",
            "./tool",
            source_request,
            root=self.repo,
            desktop_source_root_fn=lambda _target, _request: prepared_root,
            local_worktree_matches_fn=lambda _path, _sha: True,
            reset_local_worktree_fn=lambda _path: self.fail("reset should not run for a reused worktree"),
            run_fn=fake_run,
            run_logged_command_fn=lambda *args, **kwargs: logged.append((args, kwargs)),
            tail_lines_fn=lambda _path, limit=40: ["tail"],
            rewrite_launch_command_for_source_root_fn=lambda command, root: f"{root}:{command}",
        )

        self.assertEqual(reused["prepared_state"], "reused")
        self.assertEqual(reused["launch_command"], f"{prepared_root}:./tool")
        self.assertEqual(logged, [])


if __name__ == "__main__":
    unittest.main()
