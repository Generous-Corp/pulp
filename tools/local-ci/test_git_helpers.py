#!/usr/bin/env python3
"""Tests for the local-ci git/time helper seam."""

from __future__ import annotations

import importlib.util
import pathlib
import subprocess
import tempfile
import unittest
from datetime import datetime
from unittest import mock


MODULE_PATH = pathlib.Path(__file__).with_name("git_helpers.py")


def load_module():
    spec = importlib.util.spec_from_file_location("git_helpers_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


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


if __name__ == "__main__":
    unittest.main()
