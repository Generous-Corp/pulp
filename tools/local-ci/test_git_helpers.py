#!/usr/bin/env python3
"""Tests for the local-ci git/time helper seam."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest
from datetime import datetime
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("git_helpers.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class GitHelpersTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_now_iso_is_timezone_aware(self) -> None:
        parsed = datetime.fromisoformat(self.mod.now_iso())

        self.assertIsNotNone(parsed.tzinfo)

    def test_current_branch_and_sha_strip_git_output(self) -> None:
        results = [
            subprocess.CompletedProcess(["git"], 0, stdout="feature/test\n", stderr=""),
            subprocess.CompletedProcess(["git"], 0, stdout=("a" * 40) + "\n", stderr=""),
        ]

        with mock.patch.object(self.mod.subprocess, "run", side_effect=results) as run:
            self.assertEqual(self.mod.current_branch(), "feature/test")
            self.assertEqual(self.mod.current_sha(), "a" * 40)

        self.assertEqual(run.call_count, 2)
        self.assertEqual(run.call_args_list[0].kwargs["cwd"], self.mod.ROOT)
        self.assertTrue(run.call_args_list[0].kwargs["check"])
        self.assertEqual(run.call_args_list[0].args[0], ["git", "rev-parse", "--abbrev-ref", "HEAD"])
        self.assertEqual(run.call_args_list[1].args[0], ["git", "rev-parse", "HEAD"])
        self.assertTrue(run.call_args_list[1].kwargs["check"])

    def test_git_root_for_returns_resolved_path_or_none(self) -> None:
        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["git"], 0, stdout=f"{self.root}\n", stderr=""),
        ):
            self.assertEqual(self.mod.git_root_for(self.root / "subdir"), self.root.resolve())

        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["git"], 128, stdout="", stderr="fatal"),
        ) as run:
            self.assertIsNone(self.mod.git_root_for(self.root))
        self.assertEqual(run.call_args.args[0], ["git", "rev-parse", "--show-toplevel"])
        self.assertEqual(run.call_args.kwargs["cwd"], self.root)
        self.assertFalse(run.call_args.kwargs.get("check", False))

    def test_resolve_git_ref_sha_returns_commit_or_raises_detail(self) -> None:
        sha = "b" * 40
        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["git"], 0, stdout=f"{sha}\n", stderr=""),
        ) as run:
            self.assertEqual(self.mod.resolve_git_ref_sha("HEAD"), sha)
        self.assertEqual(run.call_args.args[0], ["git", "rev-parse", "--verify", "HEAD^{commit}"])
        self.assertEqual(run.call_args.kwargs["cwd"], self.mod.ROOT)

        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["git"], 1, stdout="stdout detail\n", stderr=""),
        ):
            with self.assertRaisesRegex(ValueError, "stdout detail"):
                self.mod.resolve_git_ref_sha("missing")

        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["git"], 1, stdout="", stderr="bad ref\n"),
        ):
            with self.assertRaisesRegex(ValueError, "bad ref"):
                self.mod.resolve_git_ref_sha("missing")

    def test_resolve_git_ref_sha_uses_unknown_ref_fallback(self) -> None:
        with mock.patch.object(
            self.mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["git"], 1, stdout="", stderr=""),
        ):
            with self.assertRaisesRegex(ValueError, "unknown ref"):
                self.mod.resolve_git_ref_sha("missing")

    def test_short_sha_handles_empty_and_long_values(self) -> None:
        self.assertEqual(self.mod.short_sha(""), "?")
        self.assertEqual(self.mod.short_sha(None), "?")
        self.assertEqual(self.mod.short_sha("123"), "123")
        self.assertEqual(self.mod.short_sha("1234567890abcdef"), "1234567890ab")

    def test_normalize_git_remotes_for_http_and_clone_urls(self) -> None:
        self.assertEqual(
            self.mod.normalize_git_remote_for_http("git@github.com:danielraffel/pulp.git"),
            "https://github.com/danielraffel/pulp",
        )
        self.assertEqual(
            self.mod.normalize_git_remote_for_http("https://github.com/danielraffel/pulp.git"),
            "https://github.com/danielraffel/pulp",
        )
        self.assertEqual(
            self.mod.normalize_git_remote_for_http("http://github.com/danielraffel/pulp"),
            "https://github.com/danielraffel/pulp",
        )
        self.assertIsNone(self.mod.normalize_git_remote_for_http("/tmp/pulp.git"))

        self.assertEqual(
            self.mod.normalize_git_remote_for_clone("git@github.com:danielraffel/pulp.git"),
            "https://github.com/danielraffel/pulp.git",
        )
        self.assertEqual(
            self.mod.normalize_git_remote_for_clone("https://github.com/danielraffel/pulp"),
            "https://github.com/danielraffel/pulp.git",
        )
        self.assertEqual(
            self.mod.normalize_git_remote_for_clone("https://github.com/danielraffel/pulp.git"),
            "https://github.com/danielraffel/pulp.git",
        )
        self.assertIsNone(self.mod.normalize_git_remote_for_clone("/tmp/pulp.git"))

    def test_git_origin_url_helpers_return_normalized_urls_or_none(self) -> None:
        ok = subprocess.CompletedProcess(
            ["git"],
            0,
            stdout="git@github.com:danielraffel/pulp.git\n",
            stderr="",
        )
        calls = []

        def run(cmd, **kwargs):
            calls.append((cmd, kwargs))
            return ok

        self.assertEqual(self.mod.git_origin_url(self.root, run_fn=run), "git@github.com:danielraffel/pulp.git")
        self.assertEqual(self.mod.git_origin_http_url(self.root, run_fn=run), "https://github.com/danielraffel/pulp")
        self.assertEqual(self.mod.git_origin_clone_url(self.root, run_fn=run), "https://github.com/danielraffel/pulp.git")
        self.assertTrue(all(call[0] == ["git", "remote", "get-url", "origin"] for call in calls))
        self.assertTrue(all(call[1]["cwd"] == self.root for call in calls))

        fail = subprocess.CompletedProcess(["git"], 1, stdout="", stderr="missing")
        self.assertIsNone(self.mod.git_origin_url(self.root, run_fn=lambda *args, **kwargs: fail))
        self.assertIsNone(self.mod.git_origin_http_url(self.root, run_fn=lambda *args, **kwargs: fail))
        self.assertIsNone(self.mod.git_origin_clone_url(self.root, run_fn=lambda *args, **kwargs: fail))

    def test_run_git_returns_completed_process_or_raises_detail(self) -> None:
        ok = subprocess.CompletedProcess(["git"], 0, stdout="ok", stderr="")
        fail = subprocess.CompletedProcess(["git"], 2, stdout="", stderr="bad")

        with mock.patch.object(self.mod.subprocess, "run", return_value=ok) as run:
            self.assertIs(self.mod.run_git(["status"], cwd=self.root), ok)
        self.assertEqual(run.call_args.args[0], ["git", "status"])
        self.assertEqual(run.call_args.kwargs["cwd"], self.root)
        self.assertFalse(run.call_args.kwargs["check"])

        self.assertIs(
            self.mod.run_git(["status"], cwd=self.root, check=False, run_fn=lambda *args, **kwargs: fail),
            fail,
        )
        with self.assertRaisesRegex(RuntimeError, "bad"):
            self.mod.run_git(["status"], cwd=self.root, run_fn=lambda *args, **kwargs: fail)


if __name__ == "__main__":
    unittest.main()
