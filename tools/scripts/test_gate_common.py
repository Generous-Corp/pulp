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

    def test_bare_double_star_matches_everything(self) -> None:
        self.assertTrue(gc.glob_match("a/b/c.txt", "**"))
        self.assertTrue(gc.glob_match("file.txt", "**"))

    def test_trailing_double_star_preserves_prefix_boundary(self) -> None:
        self.assertTrue(gc.glob_match("tools/cli", "tools/cli/**"))
        self.assertTrue(gc.glob_match("tools/cli/sub/file.cpp", "tools/cli/**"))
        self.assertFalse(gc.glob_match("tools/clicmd.cpp", "tools/cli/**"))

    def test_glob_to_regex_returns_compiled_pattern(self) -> None:
        p = gc.glob_to_regex("a/**")
        self.assertIsInstance(p, re.Pattern)

    def test_matches_any(self) -> None:
        self.assertTrue(gc.matches_any("a/b.cpp", ["x/**", "a/*.cpp"]))
        self.assertFalse(gc.matches_any("a/b.cpp", ["x/**", "y/**"]))

    def test_matches_any_normalizes_os_separators(self) -> None:
        win_path = "tools\\scripts\\gate_common.py"
        self.assertTrue(gc.matches_any(win_path, ["tools/scripts/*.py"]))

    def test_double_star_middle_after_literal_without_slash(self) -> None:
        # Defensive coverage for odd but supported consecutive-slash inputs.
        self.assertTrue(gc.glob_match("a/b.cpp", "a//**/*.cpp"))

    def test_double_star_trailing_after_empty_segment(self) -> None:
        self.assertTrue(gc.glob_match("a", "a//**"))
        self.assertTrue(gc.glob_match("a/b", "a//**"))

    def test_literal_regex_characters_are_escaped(self) -> None:
        self.assertTrue(gc.glob_match("a/file[1].cpp", "a/file[1].cpp"))
        self.assertFalse(gc.glob_match("a/file1.cpp", "a/file[1].cpp"))

    def test_matches_any_empty_patterns_is_false(self) -> None:
        self.assertFalse(gc.matches_any("a/b.cpp", []))


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

    def test_git_diff_names_preserves_unstripped_nonblank_lines(self) -> None:
        completed = subprocess.CompletedProcess([], 0, stdout=" a.txt \n")
        with mock.patch.object(gc.subprocess, "run", return_value=completed):
            self.assertEqual(gc.git_diff_names("main", "HEAD"), [" a.txt "])

    def test_git_diff_names_uses_three_dot_merge_base_range(self) -> None:
        """The diff must be three-dot (merge-base), not two-dot tree-compare — else a
        branch behind base on unrelated files gets them falsely attributed."""
        completed = subprocess.CompletedProcess([], 0, stdout="")
        with mock.patch.object(gc.subprocess, "run", return_value=completed) as run:
            gc.git_diff_names("origin/main", "HEAD")
        argv = run.call_args.args[0]
        self.assertIn("origin/main...HEAD", argv)
        self.assertNotIn("origin/main..HEAD", argv)

    def test_git_diff_names_ignores_files_branch_is_behind_on(self) -> None:
        """Integration: a branch that adds only Y, while base advances with X, must
        report ONLY Y (not X). This is the bug the three-dot diff fixes."""
        import os
        import tempfile

        def run_git(cwd, *args):
            subprocess.run(["git", *args], cwd=cwd, check=True,
                           capture_output=True, text=True)

        with tempfile.TemporaryDirectory() as d:
            run_git(d, "init", "-q", "-b", "main")
            run_git(d, "config", "user.email", "t@t")
            run_git(d, "config", "user.name", "t")
            (open(os.path.join(d, "base.txt"), "w")).write("base\n")
            run_git(d, "add", "."); run_git(d, "commit", "-qm", "base")
            run_git(d, "checkout", "-qb", "feature")
            (open(os.path.join(d, "Y.txt"), "w")).write("y\n")  # the PR's real change
            run_git(d, "add", "."); run_git(d, "commit", "-qm", "add Y")
            run_git(d, "checkout", "-q", "main")
            (open(os.path.join(d, "X.txt"), "w")).write("x\n")  # an unrelated later merge
            run_git(d, "add", "."); run_git(d, "commit", "-qm", "add X")

            cwd = os.getcwd()
            try:
                os.chdir(d)
                changed = gc.git_diff_names("main", "feature")
            finally:
                os.chdir(cwd)
            self.assertEqual(changed, ["Y.txt"])  # NOT X.txt (branch is behind on it)

    def test_parse_trailer_block_ignores_non_trailer_output(self) -> None:
        completed = subprocess.CompletedProcess(
            [], 0, stdout="not a trailer\nAlso not one\nKey: value\n",
        )
        with mock.patch.object(gc.subprocess, "run", return_value=completed):
            self.assertEqual(gc._parse_trailer_block("body"), {"key": ["value"]})


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

    def test_commit_trailers_returns_empty_on_git_failure(self) -> None:
        def boom(cmd, *args, **kwargs):
            raise subprocess.CalledProcessError(128, cmd)

        with mock.patch.object(gc.subprocess, "run", side_effect=boom):
            self.assertEqual(gc.git_commit_trailers("missing"), {})

    def test_range_trailers_skips_empty_chunks(self) -> None:
        def fake_run(cmd, *args, **kwargs):
            if cmd[:2] == ["git", "log"]:
                return subprocess.CompletedProcess(cmd, 0, stdout="\x00\n\x00")
            if cmd[:2] == ["git", "interpret-trailers"]:
                raise AssertionError("empty chunks should not be parsed")
            raise AssertionError(f"unexpected git call: {cmd}")

        with mock.patch.object(gc.subprocess, "run", side_effect=fake_run):
            self.assertEqual(gc.git_range_trailers("main", "HEAD"), {})


class SquashBuriedBypassTrailerTests(unittest.TestCase):
    """Regression for the merge-queue SQUASH + COMMIT_MESSAGES trailer void:
    GitHub concatenates commit bodies and appends a `--------- / Co-authored-by`
    block, so `git interpret-trailers --parse` (final-block only) drops bypass
    trailers that land mid-body. gate_common must still see them, or every
    skip-trailer PR is added→evicted on the queue in a loop."""

    def test_final_block_bypass_still_parsed(self) -> None:
        body = (
            "docs: sweep coordinate\n\n"
            "Skill-Update: skip skill=ci reason=\"coordinate-only\"\n"
            "Co-authored-by: Someone <s@example.com>\n"
        )
        got = gc._parse_trailer_block(body)
        self.assertIn("skill-update", got)
        self.assertEqual(got["skill-update"], ['skip skill=ci reason="coordinate-only"'])

    def test_squash_buried_bypass_is_rescued(self) -> None:
        # The exact shape GitHub's squash produces: trailers mid-body, then a
        # separator + Co-authored-by final block that interpret-trailers latches.
        body = (
            "docs: sweep coordinate (#6395)\n\n"
            "* docs: point coordinate at org\n\n"
            "Skill-Update: skip skill=ci reason=\"coordinate-only\"\n"
            "Config-Doc: skip reason=\"rename only\"\n\n"
            "---------\n\n"
            "Co-authored-by: Someone <s@example.com>\n"
        )
        got = gc._parse_trailer_block(body)
        self.assertIn("skill-update", got, "buried Skill-Update must be rescued")
        self.assertIn("config-doc", got, "buried Config-Doc must be rescued")
        self.assertEqual(got["skill-update"], ['skip skill=ci reason="coordinate-only"'])

    def test_rescue_is_key_agnostic(self) -> None:
        # KEY-AGNOSTIC: every bypass trailer a gate might read must survive the
        # squash, not just a hand-maintained allowlist — including the ones the
        # 5-key first cut missed (Compat-Update, Custom-Bump, Docs-Update) and
        # any future key. Gates only ever `trailers.get("<specific>")`, so
        # rescuing every trailer-shaped line is safe and drift-proof.
        body = (
            "feat: thing (#1)\n\n"
            "Compat-Update: skip reason=\"parts identical\"\n"
            "Custom-Bump: sdk=minor\n"
            "Docs-Update: skip reason=\"no doc surface\"\n"
            "Planning-Bump: skip reason=\"hypothetical future gate\"\n\n"
            "---------\n\n"
            "Co-authored-by: Someone <s@example.com>\n"
        )
        got = gc._parse_trailer_block(body)
        for key in ("compat-update", "custom-bump", "docs-update", "planning-bump"):
            self.assertIn(key, got, f"buried {key} must be rescued (key-agnostic)")

    def test_non_trailer_shapes_ignored(self) -> None:
        # The `^`-anchor + mandatory space after the colon means only real
        # trailer lines match — URLs, markdown headings, and space-bearing keys
        # (which git never treats as trailer tokens) must NOT be surfaced.
        body = (
            "feat: thing\n\n"
            "See https://example.com/x for details\n"
            "## Notes: heading, not a trailer\n"
            "BREAKING CHANGE: space in the key\n"
            "http://raw.example.com/a:b\n\n"
            "---------\n\n"
            "Co-authored-by: Someone <s@example.com>\n"
        )
        got = gc._parse_trailer_block(body)
        self.assertNotIn("breaking change", got)
        self.assertNotIn("http", got)
        self.assertNotIn("https", got)
        # A gate only reads specific keys, so even if some benign token slipped
        # in it would be inert — but confirm the URL/heading noise stays out.
        self.assertNotIn("## notes", got)


if __name__ == "__main__":
    unittest.main()
