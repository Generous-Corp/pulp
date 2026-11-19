#!/usr/bin/env python3
"""Tests for clock_in_tests_lint.py."""

from __future__ import annotations

import importlib.util
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("clock_in_tests_lint", HERE / "clock_in_tests_lint.py")
lint = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = lint
SPEC.loader.exec_module(lint)

NO_BASELINE = ({}, set())
TEST_FILE = "tools/x/test_thing.py"


def scan(text: str, baseline=NO_BASELINE, rel: str = TEST_FILE) -> list[str]:
    return lint.scan_lines(rel, list(enumerate(text.splitlines(), 1)), baseline)


class FlaggedTest(unittest.TestCase):
    def test_each_clock_read_is_flagged(self):
        for expr in (
            "datetime.now()", "dt.datetime.now(tz)", "datetime.today()", "datetime.utcnow()",  # clock-ok: fixture text
            "date.today()", "time.time()", "time.time_ns()", "time.localtime()",  # clock-ok: fixture text
        ):
            with self.subTest(expr=expr):
                self.assertEqual(len(scan(f"x = {expr}")), 1)

    def test_marker_with_reason_passes(self):
        self.assertEqual(scan("t = time.time()  # clock-ok: elapsed measurement"), [])  # clock-ok: fixture text

    def test_marker_without_reason_still_fails(self):
        self.assertEqual(len(scan("t = time.time()  # clock-ok:")), 1)  # clock-ok: fixture text

    def test_marker_in_string_literal_does_not_bypass_lint(self):
        self.assertEqual(len(scan('t = time.time() + "clock-ok: fake"')), 1)  # clock-ok: fixture text

    def test_comment_only_and_clock_free_lines_pass(self):
        self.assertEqual(scan("# uses datetime.now() in prose\nx = time.monotonic()"), [])  # clock-ok: fixture text

    def test_baselined_line_passes_and_changed_line_fails(self):
        base = ({}, {(TEST_FILE, "x = date.today()")})  # clock-ok: fixture text
        self.assertEqual(scan("    x = date.today()", base), [])  # clock-ok: fixture text
        self.assertEqual(len(scan("x = date.today() + one", base)), 1)  # clock-ok: fixture text

    def test_baselined_file_still_scans_new_lines(self):
        self.assertEqual(len(scan("x = date.today()", ({TEST_FILE: "reason"}, set()))), 1)  # clock-ok: fixture text

    def test_scope(self):
        self.assertTrue(lint.in_scope("tools/a/b/test_x.py"))
        self.assertTrue(lint.in_scope("tools/a/x_test.py"))
        self.assertFalse(lint.in_scope("tools/a/x.py"))
        self.assertFalse(lint.in_scope("core/a/test_x.py"))

    def test_shipped_baseline_covers_whole_tree(self):
        self.assertEqual(subprocess.run(
            [sys.executable, str(HERE / "clock_in_tests_lint.py"), "--all"],
            capture_output=True, text=True, encoding="utf-8").returncode, 0)


class DiffScopeTest(unittest.TestCase):
    """End to end on a scratch repo: only added lines are judged."""

    def test_planted_line_fails_and_existing_line_does_not(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            git = lambda *a: subprocess.run(["git", "-C", tmp, *a], check=True, capture_output=True)
            git("init", "-q", "-b", "main")
            git("config", "user.email", "t@example.com")
            git("config", "user.name", "t")
            (repo / "tools").mkdir()
            f = repo / "tools" / "test_a.py"
            f.write_text("old = time.time()\n", encoding="utf-8")  # clock-ok: fixture text
            git("add", "."); git("commit", "-qm", "base")
            f.write_text("old = time.time()\nnew = datetime.now()\n", encoding="utf-8")  # clock-ok: fixture text
            git("commit", "-qam", "plant")
            old_root = lint.ROOT
            lint.ROOT = repo
            try:
                added = lint.added_lines("HEAD~1", "HEAD")
            finally:
                lint.ROOT = old_root
            self.assertEqual(added, {"tools/test_a.py": [(2, "new = datetime.now()")]})  # clock-ok: fixture text

    def test_head_only_baseline_cannot_exempt_new_clock_read(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            git = lambda *a: subprocess.run(["git", "-C", tmp, *a], check=True, capture_output=True)
            git("init", "-q", "-b", "main")
            git("config", "user.email", "t@example.com")
            git("config", "user.name", "t")
            (repo / "tools" / "scripts").mkdir(parents=True)
            (repo / "tools" / "test_a.py").write_text("old = 1\n", encoding="utf-8")
            git("add", "."); git("commit", "-qm", "base")
            (repo / "tools" / "test_a.py").write_text("old = 1\nnew = datetime.now()\n", encoding="utf-8")  # clock-ok: fixture text
            (repo / "tools" / "scripts" / "clock_in_tests_baseline.json").write_text(
                '{"files": [], "lines": [{"path": "tools/test_a.py", "line": "new = datetime.now()", "reason": "bad"}]}\n',  # clock-ok: fixture text
                encoding="utf-8")
            (repo / "tools" / "scripts" / "clock_in_tests_lint.py").write_text(
                (HERE / "clock_in_tests_lint.py").read_text(encoding="utf-8"), encoding="utf-8")
            git("add", "."); git("commit", "-qm", "plant")
            result = subprocess.run(
                [sys.executable, str(repo / "tools" / "scripts" / "clock_in_tests_lint.py"),
                 "--base", "HEAD~1", "--head", "HEAD"], cwd=repo,
                capture_output=True, text=True, encoding="utf-8")
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)

    def test_unary_plus_clock_read_and_following_line_are_scanned(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            git = lambda *a: subprocess.run(["git", "-C", tmp, *a], check=True, capture_output=True)
            git("init", "-q", "-b", "main")
            git("config", "user.email", "t@example.com"); git("config", "user.name", "t")
            (repo / "tools").mkdir()
            f = repo / "tools" / "test_a.py"
            f.write_text("old = 1\n", encoding="utf-8"); git("add", "."); git("commit", "-qm", "base")
            f.write_text("old = 1\n++ b/x\nnext = time.time()\n", encoding="utf-8")  # clock-ok: fixture text
            git("commit", "-qam", "plant")
            old_root = lint.ROOT; lint.ROOT = repo
            try:
                added = lint.added_lines("HEAD~1", "HEAD")
            finally:
                lint.ROOT = old_root
            self.assertEqual([n for n, _ in added["tools/test_a.py"]], [2, 3])

    def test_renamed_test_destination_is_scanned(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            git = lambda *a: subprocess.run(["git", "-C", tmp, *a], check=True, capture_output=True)
            git("init", "-q", "-b", "main")
            git("config", "user.email", "t@example.com"); git("config", "user.name", "t")
            (repo / "tools").mkdir(); old = repo / "tools" / "test_a.py"
            old.write_text("old = 1\n", encoding="utf-8"); git("add", "."); git("commit", "-qm", "base")
            new = repo / "tools" / "test_b.py"; old.rename(new)
            new.write_text("old = 1\nnew = datetime.now()\n", encoding="utf-8")  # clock-ok: fixture text
            git("add", "."); git("commit", "-qm", "rename")
            old_root = lint.ROOT; lint.ROOT = repo
            try:
                added = lint.added_lines("HEAD~1", "HEAD")
            finally:
                lint.ROOT = old_root
            self.assertIn("tools/test_b.py", added)


if __name__ == "__main__":
    unittest.main()
