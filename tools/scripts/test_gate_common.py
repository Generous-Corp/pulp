#!/usr/bin/env python3
"""Tests for gate_common.py — the shared substrate used by
version_bump_check, compat_sync_check, and skill_sync_check.

This is the single source of truth for glob translation and git
trailer collection across the three gate scripts. The gate-specific
tests cover end-to-end behavior; these tests pin the substrate's
contract so a regression here surfaces with a focused diagnostic
instead of an unrelated gate test failing.
"""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys
import unittest
from unittest import mock

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools" / "scripts"))
import gate_common as gc  # noqa: E402


class GlobToRegexTests(unittest.TestCase):
    """Pin the post-#554 slash-boundary semantics that the version-bump
    incident exposed."""

    def test_single_star_within_segment(self) -> None:
        self.assertTrue(gc.glob_match("a/b.cpp", "a/*.cpp"))
        self.assertFalse(gc.glob_match("a/sub/b.cpp", "a/*.cpp"))

    def test_question_mark_single_char(self) -> None:
        self.assertTrue(gc.glob_match("a/b.cpp", "a/?.cpp"))
        self.assertFalse(gc.glob_match("a/bb.cpp", "a/?.cpp"))

    def test_double_star_spans_zero_segments(self) -> None:
        # The bug fixed in #554 — trailing ** must match the zero-segment case.
        self.assertTrue(gc.glob_match("a", "a/**"))
        self.assertTrue(gc.glob_match("a/b/c.cpp", "a/**"))

    def test_double_star_middle_segment_does_not_collapse_boundary(self) -> None:
        # The exact regression that motivated the fix: tools/cli/**/*.cpp
        # must NOT match tools/clicmd.cpp because '**' collapsing to zero
        # segments should still preserve the surrounding '/'.
        self.assertFalse(gc.glob_match("tools/clicmd.cpp", "tools/cli/**/*.cpp"))
        self.assertTrue(gc.glob_match("tools/cli/cmd_doctor.cpp", "tools/cli/**/*.cpp"))
        self.assertTrue(gc.glob_match("tools/cli/sub/cmd_doctor.cpp", "tools/cli/**/*.cpp"))

    def test_leading_double_star(self) -> None:
        self.assertTrue(gc.glob_match("a/b.cpp", "**/*.cpp"))
        self.assertTrue(gc.glob_match("b.cpp", "**/*.cpp"))

    def test_glob_to_regex_returns_compiled_pattern(self) -> None:
        p = gc.glob_to_regex("a/**")
        self.assertIsInstance(p, re.Pattern)

    def test_matches_any(self) -> None:
        self.assertTrue(gc.matches_any("a/b.cpp", ["x/**", "a/*.cpp"]))
        self.assertFalse(gc.matches_any("a/b.cpp", ["x/**", "y/**"]))


class StripMetaTests(unittest.TestCase):
    def test_strips_underscore_and_schema_keys(self) -> None:
        self.assertEqual(
            gc.strip_meta({"_comment": "x", "$schema": "u", "real": 1}),
            {"real": 1},
        )

    def test_non_dict_passes_through(self) -> None:
        self.assertEqual(gc.strip_meta(["a", "b"]), ["a", "b"])
        self.assertEqual(gc.strip_meta("hello"), "hello")
        self.assertEqual(gc.strip_meta(None), None)


class GitHelperTests(unittest.TestCase):
    def test_repo_root_delegates_to_git(self) -> None:
        completed = subprocess.CompletedProcess(
            [], 0, stdout="/repo/path\n",
        )
        with mock.patch.object(gc.subprocess, "run", return_value=completed) as run:
            self.assertEqual(gc.repo_root(), pathlib.Path("/repo/path"))
        run.assert_called_once_with(
            ["git", "rev-parse", "--show-toplevel"],
            check=True, capture_output=True, text=True,
        )

    def test_git_diff_names_splits_lines_and_drops_blanks(self) -> None:
        completed = subprocess.CompletedProcess(
            [], 0, stdout="a.txt\nb.txt\n\n\nc.txt\n",
        )
        with mock.patch.object(gc.subprocess, "run", return_value=completed):
            self.assertEqual(
                gc.git_diff_names("main", "HEAD"),
                ["a.txt", "b.txt", "c.txt"],
            )


class TrailerParseTests(unittest.TestCase):
    """git_range_trailers + git_commit_trailers share the same parse
    helper. Cover the join + filter behavior here so future refactors
    can't silently drop trailers from middle commits in a range."""

    def _fake_run(self, log_stdout: str, parse_stdout: str):
        log_call = {"count": 0}
        parse_call = {"count": 0}

        def run(cmd, *args, **kwargs):
            if cmd[:2] == ["git", "log"]:
                log_call["count"] += 1
                return subprocess.CompletedProcess(cmd, 0, stdout=log_stdout)
            if cmd[:2] == ["git", "interpret-trailers"]:
                parse_call["count"] += 1
                return subprocess.CompletedProcess(cmd, 0, stdout=parse_stdout)
            raise AssertionError(f"unexpected git call: {cmd}")

        return run, log_call, parse_call

    def test_range_trailers_walks_every_commit_body(self) -> None:
        # Two commits in the range, NUL-separated.
        log_stdout = "subj1\n\nSkill-Update: skip skill=foo reason=\"x\"\n\x00subj2\n\nVersion-Bump: skip reason=\"y\"\n\x00"

        def fake_run(cmd, *args, **kwargs):
            if cmd[:2] == ["git", "log"]:
                return subprocess.CompletedProcess(cmd, 0, stdout=log_stdout)
            if cmd[:2] == ["git", "interpret-trailers"]:
                stdin = kwargs.get("input", "")
                if "Skill-Update" in stdin:
                    return subprocess.CompletedProcess(
                        cmd, 0,
                        stdout="Skill-Update: skip skill=foo reason=\"x\"\n",
                    )
                if "Version-Bump" in stdin:
                    return subprocess.CompletedProcess(
                        cmd, 0,
                        stdout="Version-Bump: skip reason=\"y\"\n",
                    )
                return subprocess.CompletedProcess(cmd, 0, stdout="")
            raise AssertionError(f"unexpected git call: {cmd}")

        with mock.patch.object(gc.subprocess, "run", side_effect=fake_run):
            trailers = gc.git_range_trailers("main", "HEAD")
        self.assertIn("skill-update", trailers)
        self.assertIn("version-bump", trailers)

    def test_range_trailers_returns_empty_on_git_failure(self) -> None:
        def boom(cmd, *args, **kwargs):
            raise subprocess.CalledProcessError(128, cmd)

        with mock.patch.object(gc.subprocess, "run", side_effect=boom):
            self.assertEqual(gc.git_range_trailers("main", "HEAD"), {})

    def test_commit_trailers_reads_single_ref(self) -> None:
        def fake_run(cmd, *args, **kwargs):
            if cmd[:3] == ["git", "log", "-1"]:
                return subprocess.CompletedProcess(
                    cmd, 0,
                    stdout="subject\n\nCompat-Update: skip prefix=css reason=\"ok\"\n",
                )
            if cmd[:2] == ["git", "interpret-trailers"]:
                return subprocess.CompletedProcess(
                    cmd, 0,
                    stdout="Compat-Update: skip prefix=css reason=\"ok\"\n",
                )
            raise AssertionError(f"unexpected: {cmd}")

        with mock.patch.object(gc.subprocess, "run", side_effect=fake_run):
            t = gc.git_commit_trailers("abc123")
        self.assertEqual(
            t["compat-update"], ["skip prefix=css reason=\"ok\""],
        )


if __name__ == "__main__":
    unittest.main()
