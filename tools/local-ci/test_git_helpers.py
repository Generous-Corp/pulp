#!/usr/bin/env python3
"""Tests for the local-ci git/time helper seams."""

from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest
from datetime import datetime
from unittest import mock

from module_test_utils import load_module_from_path


COMMAND_MODULE_PATH = pathlib.Path(__file__).with_name("git_command_helpers.py")
COMPAT_MODULE_PATH = pathlib.Path(__file__).with_name("git_helpers.py")
REF_MODULE_PATH = pathlib.Path(__file__).with_name("git_ref_helpers.py")
REMOTE_MODULE_PATH = pathlib.Path(__file__).with_name("git_remote_helpers.py")
TIME_MODULE_PATH = pathlib.Path(__file__).with_name("git_time_helpers.py")


def load_module(path: pathlib.Path):
    return load_module_from_path(path, add_module_dir=True)


class GitHelpersTests(unittest.TestCase):
    def setUp(self) -> None:
        self.command_mod = load_module(COMMAND_MODULE_PATH)
        self.compat_mod = load_module(COMPAT_MODULE_PATH)
        self.ref_mod = load_module(REF_MODULE_PATH)
        self.remote_mod = load_module(REMOTE_MODULE_PATH)
        self.time_mod = load_module(TIME_MODULE_PATH)
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_now_iso_is_timezone_aware(self) -> None:
        parsed = datetime.fromisoformat(self.time_mod.now_iso())

        self.assertIsNotNone(parsed.tzinfo)

    def test_current_branch_and_sha_strip_git_output(self) -> None:
        results = [
            subprocess.CompletedProcess(["git"], 0, stdout="feature/test\n", stderr=""),
            subprocess.CompletedProcess(["git"], 0, stdout=("a" * 40) + "\n", stderr=""),
        ]

        with mock.patch.object(self.ref_mod.subprocess, "run", side_effect=results) as run:
            self.assertEqual(self.ref_mod.current_branch(), "feature/test")
            self.assertEqual(self.ref_mod.current_sha(), "a" * 40)

        self.assertEqual(run.call_count, 2)
        self.assertEqual(run.call_args_list[0].kwargs["cwd"], self.ref_mod.ROOT)
        self.assertTrue(run.call_args_list[0].kwargs["check"])
        self.assertEqual(run.call_args_list[0].args[0], ["git", "rev-parse", "--abbrev-ref", "HEAD"])
        self.assertEqual(run.call_args_list[1].args[0], ["git", "rev-parse", "HEAD"])
        self.assertTrue(run.call_args_list[1].kwargs["check"])

    def test_git_root_for_returns_resolved_path_or_none(self) -> None:
        with mock.patch.object(
            self.ref_mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["git"], 0, stdout=f"{self.root}\n", stderr=""),
        ):
            self.assertEqual(self.ref_mod.git_root_for(self.root / "subdir"), self.root.resolve())

        with mock.patch.object(
            self.ref_mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["git"], 128, stdout="", stderr="fatal"),
        ) as run:
            self.assertIsNone(self.ref_mod.git_root_for(self.root))
        self.assertEqual(run.call_args.args[0], ["git", "rev-parse", "--show-toplevel"])
        self.assertEqual(run.call_args.kwargs["cwd"], self.root)
        self.assertFalse(run.call_args.kwargs.get("check", False))

    def test_resolve_git_ref_sha_returns_commit_or_raises_detail(self) -> None:
        sha = "b" * 40
        with mock.patch.object(
            self.ref_mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["git"], 0, stdout=f"{sha}\n", stderr=""),
        ) as run:
            self.assertEqual(self.ref_mod.resolve_git_ref_sha("HEAD"), sha)
        self.assertEqual(run.call_args.args[0], ["git", "rev-parse", "--verify", "HEAD^{commit}"])
        self.assertEqual(run.call_args.kwargs["cwd"], self.ref_mod.ROOT)

        with mock.patch.object(
            self.ref_mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["git"], 1, stdout="stdout detail\n", stderr=""),
        ):
            with self.assertRaisesRegex(ValueError, "stdout detail"):
                self.ref_mod.resolve_git_ref_sha("missing")

        with mock.patch.object(
            self.ref_mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["git"], 1, stdout="", stderr="bad ref\n"),
        ):
            with self.assertRaisesRegex(ValueError, "bad ref"):
                self.ref_mod.resolve_git_ref_sha("missing")

    def test_resolve_git_ref_sha_uses_unknown_ref_fallback(self) -> None:
        with mock.patch.object(
            self.ref_mod.subprocess,
            "run",
            return_value=subprocess.CompletedProcess(["git"], 1, stdout="", stderr=""),
        ):
            with self.assertRaisesRegex(ValueError, "unknown ref"):
                self.ref_mod.resolve_git_ref_sha("missing")

    def test_short_sha_handles_empty_and_long_values(self) -> None:
        self.assertEqual(self.ref_mod.short_sha(""), "?")
        self.assertEqual(self.ref_mod.short_sha(None), "?")
        self.assertEqual(self.ref_mod.short_sha("123"), "123")
        self.assertEqual(self.ref_mod.short_sha("1234567890abcdef"), "1234567890ab")

    def test_normalize_git_remotes_for_http_and_clone_urls(self) -> None:
        self.assertEqual(
            self.remote_mod.normalize_git_remote_for_http("git@github.com:danielraffel/pulp.git"),
            "https://github.com/danielraffel/pulp",
        )
        self.assertEqual(
            self.remote_mod.normalize_git_remote_for_http("https://github.com/danielraffel/pulp.git"),
            "https://github.com/danielraffel/pulp",
        )
        self.assertEqual(
            self.remote_mod.normalize_git_remote_for_http("http://github.com/danielraffel/pulp"),
            "https://github.com/danielraffel/pulp",
        )
        self.assertIsNone(self.remote_mod.normalize_git_remote_for_http("/tmp/pulp.git"))

        self.assertEqual(
            self.remote_mod.normalize_git_remote_for_clone("git@github.com:danielraffel/pulp.git"),
            "https://github.com/danielraffel/pulp.git",
        )
        self.assertEqual(
            self.remote_mod.normalize_git_remote_for_clone("https://github.com/danielraffel/pulp"),
            "https://github.com/danielraffel/pulp.git",
        )
        self.assertEqual(
            self.remote_mod.normalize_git_remote_for_clone("https://github.com/danielraffel/pulp.git"),
            "https://github.com/danielraffel/pulp.git",
        )
        self.assertIsNone(self.remote_mod.normalize_git_remote_for_clone("/tmp/pulp.git"))

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

        self.assertEqual(self.remote_mod.git_origin_url(self.root, run_fn=run), "git@github.com:danielraffel/pulp.git")
        self.assertEqual(self.remote_mod.git_origin_http_url(self.root, run_fn=run), "https://github.com/danielraffel/pulp")
        self.assertEqual(
            self.remote_mod.git_origin_clone_url(self.root, run_fn=run),
            "https://github.com/danielraffel/pulp.git",
        )
        self.assertTrue(all(call[0] == ["git", "remote", "get-url", "origin"] for call in calls))
        self.assertTrue(all(call[1]["cwd"] == self.root for call in calls))

        fail = subprocess.CompletedProcess(["git"], 1, stdout="", stderr="missing")
        self.assertIsNone(self.remote_mod.git_origin_url(self.root, run_fn=lambda *args, **kwargs: fail))
        self.assertIsNone(self.remote_mod.git_origin_http_url(self.root, run_fn=lambda *args, **kwargs: fail))
        self.assertIsNone(self.remote_mod.git_origin_clone_url(self.root, run_fn=lambda *args, **kwargs: fail))

    def test_run_git_returns_completed_process_or_raises_detail(self) -> None:
        ok = subprocess.CompletedProcess(["git"], 0, stdout="ok", stderr="")
        fail = subprocess.CompletedProcess(["git"], 2, stdout="", stderr="bad")

        with mock.patch.object(self.command_mod.subprocess, "run", return_value=ok) as run:
            self.assertIs(self.command_mod.run_git(["status"], cwd=self.root), ok)
        self.assertEqual(run.call_args.args[0], ["git", "status"])
        self.assertEqual(run.call_args.kwargs["cwd"], self.root)
        self.assertFalse(run.call_args.kwargs["check"])

        self.assertIs(
            self.command_mod.run_git(["status"], cwd=self.root, check=False, run_fn=lambda *args, **kwargs: fail),
            fail,
        )
        with self.assertRaisesRegex(RuntimeError, "bad"):
            self.command_mod.run_git(["status"], cwd=self.root, run_fn=lambda *args, **kwargs: fail)

    def test_git_helpers_reexports_focused_helpers(self) -> None:
        expected_names = {
            "ROOT",
            "current_branch",
            "current_sha",
            "git_origin_clone_url",
            "git_origin_http_url",
            "git_origin_url",
            "git_root_for",
            "normalize_git_remote_for_clone",
            "normalize_git_remote_for_http",
            "now_iso",
            "resolve_git_ref_sha",
            "run_git",
            "short_sha",
        }

        self.assertEqual(set(self.compat_mod.__all__), expected_names)
        self.assertEqual(self.compat_mod.now_iso.__name__, "now_iso")
        self.assertEqual(self.compat_mod.current_branch.__name__, "current_branch")
        self.assertEqual(self.compat_mod.git_origin_url.__name__, "git_origin_url")
        self.assertEqual(self.compat_mod.run_git.__name__, "run_git")
        self.assertEqual(
            self.compat_mod.normalize_git_remote_for_clone("git@github.com:danielraffel/pulp.git"),
            "https://github.com/danielraffel/pulp.git",
        )


if __name__ == "__main__":
    unittest.main()
