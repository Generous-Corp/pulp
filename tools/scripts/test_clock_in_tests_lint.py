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
# Keep dynamic helper and baseline dependencies visible to script-test-inputs.
DECLARED_SCRIPT_INPUTS = ("tools/scripts/clock_in_tests_lint.py", "tools/scripts/clock_in_tests_baseline.json")
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
            "time . time()", "datetime . now()", "date . today()",  # clock-ok: fixture text
        ):
            with self.subTest(expr=expr):
                self.assertEqual(len(scan(f"x = {expr}")), 1)

    def test_multiline_qualified_clock_read_is_flagged(self):
        lines = [(1, "value = datetime"), (2, "    .now()"), (3, "other = time\\"), (4, "    .time()")]
        self.assertEqual(len(lint.scan_lines(TEST_FILE, lines, NO_BASELINE)), 2)  # clock-ok: fixture text

    def test_additional_local_clock_apis_are_flagged(self):
        for expr in ("time.gmtime()", "time.ctime()", "time.asctime()", "time.strftime('%Y')"):
            with self.subTest(expr=expr):
                self.assertEqual(len(scan(f"value = {expr}")), 1)

    def test_not_clock_ok_is_not_a_marker(self):
        self.assertEqual(len(scan("value = time.time()  # NOT clock-ok: explained")), 1)

    def test_f_string_prefix_variants_and_multiline_expression_are_flagged(self):
        for source in (
            "value = F'{time.time()}'",
            "value = rf'{datetime.now()}'",
            "value = fr'{date.today()}'",
            "value = f'{dt.datetime.now()}'",
            "value = f'{datetime.datetime.now()}'",
            'value = RF"""\n{time.time()}\n"""',
        ):
            with self.subTest(source=source):
                self.assertEqual(len(scan(source)), 1)
        self.assertEqual(scan("value = RF'{time.time()}'  # clock-ok: measured"), [])  # clock-ok: fixture text

    def test_marker_with_reason_passes(self):
        self.assertEqual(scan("t = time.time()  # clock-ok: elapsed measurement"), [])  # clock-ok: fixture text

    def test_marker_without_reason_still_fails(self):
        self.assertEqual(len(scan("t = time.time()  # clock-ok:")), 1)  # clock-ok: fixture text


class DiffScopeTest(unittest.TestCase):
    def test_marker_on_unchanged_line_cannot_exempt_added_multiline_call(self):
        lines = [(1, "value = time  # clock-ok: old use"), (2, "    .time()")]
        self.assertEqual(len(lint.scan_lines(TEST_FILE, lines, NO_BASELINE, added_numbers={2})), 1)

    def test_prefix_config_is_overridden_for_real_diff(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            git = lambda *a: subprocess.run(["git", "-C", tmp, *a], check=True, capture_output=True)
            git("init", "-q", "-b", "main")
            git("config", "user.email", "t@example.com"); git("config", "user.name", "t")
            (repo / "tools").mkdir(); f = repo / "tools" / "test_a.py"
            f.write_text("old = 1\n", encoding="utf-8"); git("add", "."); git("commit", "-qm", "base")
            f.write_text("old = 1\nnew = time.time()\n", encoding="utf-8"); git("commit", "-qam", "plant")  # clock-ok: fixture text
            git("config", "diff.noprefix", "true"); git("config", "diff.mnemonicPrefix", "true")
            old_root = lint.ROOT; lint.ROOT = repo
            try:
                added = lint.added_lines("HEAD~1", "HEAD")
            finally:
                lint.ROOT = old_root
            self.assertEqual(added["tools/test_a.py"], [(2, "new = time.time()")])  # clock-ok: fixture text


if __name__ == '__main__':
    unittest.main()
