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

    def test_parenthesized_multiline_call_is_flagged(self):
        lines = [(1, "value = (time."), (2, "    time())")]
        self.assertEqual(len(lint.scan_lines(TEST_FILE, lines, NO_BASELINE)), 1)  # clock-ok: fixture text

    def test_changed_method_line_is_flagged_with_unchanged_qualifier(self):
        lines = [(1, "value = time."), (2, "    time()")]
        self.assertEqual(len(lint.scan_lines(TEST_FILE, lines, NO_BASELINE, added_numbers={2})), 1)  # clock-ok: fixture text

    def test_comments_between_qualifier_and_selector_are_flagged(self):
        source = "value = (time  # qualifier\n    .time())"
        self.assertEqual(len(scan(source)), 1)  # clock-ok: fixture text

    def test_parenthesized_qualifier_is_flagged(self):
        self.assertEqual(len(scan("value = (time).time()")), 1)  # clock-ok: fixture text
        self.assertEqual(len(scan("value = ((time)).time()")), 1)  # clock-ok: fixture text
        self.assertEqual(len(scan("value = (time.time)()")), 1)  # clock-ok: fixture text

    def test_additional_local_clock_apis_are_flagged(self):
        for expr in ("time.gmtime()", "time.ctime()", "time.asctime()", "time.strftime('%Y')"):
            with self.subTest(expr=expr):
                self.assertEqual(len(scan(f"value = {expr}")), 1)

    def test_not_clock_ok_is_not_a_marker(self):
        self.assertEqual(len(scan("value = time.time()  # NOT clock-ok: explained")), 1)

    def test_comment_before_call_is_flagged(self):
        source = "value = (time.time  # ordinary comment\n    ())"
        self.assertEqual(len(scan(source)), 1)  # clock-ok: fixture text

    def test_nfkc_qualified_identifier_is_flagged(self):
        self.assertEqual(len(scan("value = 𝚝𝚒𝚖𝚎.time()")), 1)  # clock-ok: fixture text

    def test_f_string_clock_expression_is_flagged(self):
        self.assertEqual(len(scan("value = f'{time.time()}'")), 1)  # clock-ok: fixture text
        self.assertEqual(scan("value = f'{time.time()}'  # clock-ok: measured"), [])  # clock-ok: fixture text

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

    def test_added_lines_context_counter_preserves_later_attribution(self):
        original = lint._git
        calls = iter(["base\n", '+++ b/tools/test_a.py\n@@ -1,0 +1,4 @@\n+first = 1\n context = 2\n+value = time.time()\n'])
        lint._git = lambda _args: next(calls)
        try:
            added = lint.added_lines("base", "HEAD")
        finally:
            lint._git = original
        self.assertEqual(added["tools/test_a.py"], [(1, "first = 1"), (3, "value = time.time()")])
        source = [(1, "first = 1"), (2, "context = 2"), (3, "value = time.time()")]
        findings = lint.scan_lines(
            "tools/test_a.py", source, NO_BASELINE,
            added_numbers={number for number, _ in added["tools/test_a.py"]},
        )
        self.assertEqual(len(findings), 1)
        self.assertIn("tools/test_a.py:3:", findings[0])

    def test_malformed_source_fails_closed(self):
        findings = scan("value = time.time()\nvalue = (")
        self.assertEqual(len(findings), 1)  # clock-ok: fixture text
        self.assertIn("could not tokenize", findings[0])  # clock-ok: fixture text

    def test_marker_inside_multiline_string_does_not_bypass_lint(self):
        source = 's = """start\n"""\nvalue = time.time()\ntail = """# clock-ok: fake\nend"""'
        self.assertEqual(len(scan(source)), 1)  # clock-ok: fixture text

    def test_tracked_lines_preserves_non_ascii_git_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            path = root / "tools" / "test_é.py"
            path.parent.mkdir()
            path.write_text("value = 1\n", encoding="utf-8")
            old_root, old_git = lint.ROOT, lint._git
            lint.ROOT = root
            lint._git = lambda _args: "tools/test_é.py\0"
            try:
                self.assertIn("tools/test_é.py", lint.tracked_lines())
            finally:
                lint.ROOT, lint._git = old_root, old_git

    def test_tracked_lines_fails_closed_on_committed_read_error(self):
        original = lint._git
        def failing_git(args):
            if args[:2] == ["show", "HEAD:tools/test_é.py"]:
                raise subprocess.CalledProcessError(128, args)
            return "tools/test_é.py\0"
        lint._git = failing_git
        try:
            with self.assertRaises(RuntimeError):
                lint.tracked_lines("HEAD")
        finally:
            lint._git = original

    def test_cli_uses_matching_committed_or_working_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            git = lambda *a: subprocess.run(["git", "-C", tmp, *a], check=True, capture_output=True)
            git("init", "-q", "-b", "main")
            git("config", "user.email", "t@example.com"); git("config", "user.name", "t")
            (repo / "tools" / "scripts").mkdir(parents=True)
            (repo / "tools" / "test_a.py").write_text("value = 1\n", encoding="utf-8")
            (repo / "tools" / "scripts" / "clock_in_tests_lint.py").write_text(
                (HERE / "clock_in_tests_lint.py").read_text(encoding="utf-8"), encoding="utf-8")
            (repo / "tools" / "scripts" / "clock_in_tests_baseline.json").write_text(
                '{"files": [], "lines": []}\n', encoding="utf-8")
            git("add", "."); git("commit", "-qm", "base")
            (repo / "tools" / "test_a.py").write_text("value = time.time()\n", encoding="utf-8")
            git("add", "."); git("commit", "-qm", "clock")
            (repo / "tools" / "test_a.py").write_text("value = 1\n", encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(repo / "tools/scripts/clock_in_tests_lint.py"),
                 "--base", "HEAD~1", "--head", "HEAD"], cwd=repo,
                capture_output=True, text=True, encoding="utf-8")
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
            git("reset", "--hard", "-q", "HEAD~1")
            (repo / "tools" / "test_a.py").write_text("value = time.time()\n", encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(repo / "tools/scripts/clock_in_tests_lint.py"),
                 "--base", "HEAD"], cwd=repo,
                capture_output=True, text=True, encoding="utf-8")
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)

    def test_cli_preserves_embedded_cr_and_form_feed_records(self):
        for payload in (b"# harmless\rvalue = time.time()\n", b"value = 0\fvalue = time.time()\n"):
            with self.subTest(payload=payload):
                with tempfile.TemporaryDirectory() as tmp:
                    repo = Path(tmp)
                    git = lambda *a: subprocess.run(["git", "-C", tmp, *a], check=True, capture_output=True)
                    git("init", "-q", "-b", "main")
                    git("config", "user.email", "t@example.com"); git("config", "user.name", "t")
                    (repo / "tools" / "scripts").mkdir(parents=True)
                    f = repo / "tools" / "test_a.py"; f.write_bytes(b"value = 1\n")
                    (repo / "tools" / "scripts" / "clock_in_tests_lint.py").write_text(
                        (HERE / "clock_in_tests_lint.py").read_text(encoding="utf-8"), encoding="utf-8")
                    (repo / "tools" / "scripts" / "clock_in_tests_baseline.json").write_text(
                        '{"files": [], "lines": []}\n', encoding="utf-8")
                    git("add", "."); git("commit", "-qm", "base")
                    f.write_bytes(payload); git("add", "."); git("commit", "-qm", "clock")
                    result = subprocess.run(
                        [sys.executable, str(repo / "tools/scripts/clock_in_tests_lint.py"),
                         "--base", "HEAD~1", "--head", "HEAD"], cwd=repo,
                        capture_output=True, text=True, encoding="utf-8")
                    self.assertEqual(result.returncode, 1, result.stdout + result.stderr)

    def test_cli_head_inventory_survives_staged_index_removal(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            git = lambda *a: subprocess.run(["git", "-C", tmp, *a], check=True, capture_output=True)
            git("init", "-q", "-b", "main"); git("config", "user.email", "t@example.com"); git("config", "user.name", "t")
            (repo / "tools" / "scripts").mkdir(parents=True)
            f = repo / "tools" / "test_a.py"; f.write_text("value = time.\n    monotonic()\n", encoding="utf-8")
            (repo / "tools/scripts/clock_in_tests_lint.py").write_text((HERE / "clock_in_tests_lint.py").read_text(encoding="utf-8"), encoding="utf-8")
            (repo / "tools/scripts/clock_in_tests_baseline.json").write_text('{"files": [], "lines": []}\n', encoding="utf-8")
            git("add", "."); git("commit", "-qm", "base")
            f.write_text("value = time.\n    time()\n", encoding="utf-8"); git("add", "."); git("commit", "-qm", "clock")
            git("rm", "--cached", "-q", "tools/test_a.py")
            result = subprocess.run([sys.executable, str(repo / "tools/scripts/clock_in_tests_lint.py"),
                                     "--base", "HEAD~1", "--head", "HEAD"], cwd=repo,
                                    capture_output=True, text=True, encoding="utf-8")
            self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
    def test_marker_with_reason_passes(self):
        self.assertEqual(scan("t = time.time()  # clock-ok: elapsed measurement"), [])  # clock-ok: fixture text

    def test_marker_without_reason_still_fails(self):
        self.assertEqual(len(scan("t = time.time()  # clock-ok:")), 1)  # clock-ok: fixture text

    def test_marker_in_string_literal_does_not_bypass_lint(self):
        self.assertEqual(len(scan('t = time.time() + "clock-ok: fake"')), 1)  # clock-ok: fixture text

    def test_comment_only_and_clock_free_lines_pass(self):
        self.assertEqual(scan("# uses datetime.now() in prose\nx = time.monotonic()"), [])  # clock-ok: fixture text

    def test_baselined_line_passes_and_changed_line_fails(self):
        base = ({}, {(TEST_FILE, "x = date.today()")})  # clock-ok: fixture text  # clock-ok: fixture text
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

    def test_unquoted_destination_tab_header_is_scanned(self):
        original = lint._git
        calls = iter(["base\n", '+++ b/tools/x/test_thing space.py\t\n@@ -1,0 +1,1 @@\n+x = time.time()\n'])  # clock-ok: fixture text
        lint._git = lambda _args: next(calls)
        try:
            self.assertEqual(lint.added_lines("base", "HEAD"), {
                "tools/x/test_thing space.py": [(1, "x = time.time()")]  # clock-ok: fixture text
            })
        finally:
            lint._git = original

    def test_marker_on_unchanged_line_cannot_exempt_added_multiline_call(self):
        lines = [(1, "value = time  # clock-ok: old use"), (2, "    .time()")]
        self.assertEqual(len(lint.scan_lines(TEST_FILE, lines, NO_BASELINE, added_numbers={2})), 1)

    def test_embedded_cr_before_clock_read_is_scanned(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp); git = lambda *a: subprocess.run(["git", "-C", tmp, *a], check=True, capture_output=True)
            git("init", "-q", "-b", "main"); git("config", "user.email", "t@example.com"); git("config", "user.name", "t")
            (repo / "tools").mkdir(); f = repo / "tools" / "test_cr.py"; f.write_text("old = 1\n", encoding="utf-8")
            git("add", "."); git("commit", "-qm", "base")
            f.write_bytes(b"# harmless\rvalue = time.time()\n"); git("add", "."); git("commit", "-qam", "plant")  # clock-ok: fixture text
            old_root = lint.ROOT; lint.ROOT = repo
            try: added = lint.added_lines("HEAD~1", "HEAD")
            finally: lint.ROOT = old_root
            self.assertEqual(len(lint.scan_lines("tools/test_cr.py", added["tools/test_cr.py"], NO_BASELINE)), 1)

    def test_quoted_destination_header_is_scanned(self):
        original = lint._git
        calls = iter(["base\n", '+++ "b/tools/x/test_thing space.py"\n@@ -1,0 +1,1 @@\n+x = time.time()\n'])  # clock-ok: fixture text
        lint._git = lambda _args: next(calls)
        try:
            self.assertEqual(lint.added_lines("base", "HEAD"), {
                "tools/x/test_thing space.py": [(1, "x = time.time()")]  # clock-ok: fixture text  # clock-ok: fixture text
            })
        finally:
            lint._git = original

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

    def test_duplicate_added_baseline_line_is_not_exempted_twice(self):
        base = ({}, {(TEST_FILE, "x = date.today()")})  # clock-ok: fixture text
        lines = [(1, "x = date.today()"), (2, "x = date.today()")]  # clock-ok: fixture text
        self.assertEqual(len(lint.scan_lines(TEST_FILE, lines, base, consume_baseline=True)), 2)

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

    def test_gitattributes_cannot_hide_added_clock_read(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            git = lambda *a: subprocess.run(["git", "-C", tmp, *a], check=True, capture_output=True)
            git("init", "-q", "-b", "main")
            git("config", "user.email", "t@example.com"); git("config", "user.name", "t")
            (repo / "tools").mkdir(); f = repo / "tools" / "test_a.py"
            f.write_text("old = 1\n", encoding="utf-8")
            git("add", "."); git("commit", "-qm", "base")
            (repo / ".gitattributes").write_text("tools/test_a.py -diff\n", encoding="utf-8")
            (repo / "tools" / "test_a.py").write_text("old = 1\nnew = time.time()\n", encoding="utf-8")  # clock-ok: fixture text
            git("add", "."); git("commit", "-qm", "plant")
            old_root = lint.ROOT; lint.ROOT = repo
            try:
                added = lint.added_lines("HEAD~1", "HEAD")
            finally:
                lint.ROOT = old_root
            self.assertIn("tools/test_a.py", added)
            self.assertEqual(len(lint.scan_lines("tools/test_a.py", added["tools/test_a.py"], NO_BASELINE)), 1)

    def test_type_change_to_regular_test_is_scanned(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            git = lambda *a: subprocess.run(["git", "-C", tmp, *a], check=True, capture_output=True)
            git("init", "-q", "-b", "main")
            git("config", "user.email", "t@example.com"); git("config", "user.name", "t")
            (repo / "tools").mkdir(); f = repo / "tools" / "test_a.py"
            f.symlink_to("fixture.py"); (repo / "tools" / "fixture.py").write_text("old = 1\n", encoding="utf-8")
            git("add", "."); git("commit", "-qm", "base")
            f.unlink(); f.write_text("new = time.time()\n", encoding="utf-8")  # clock-ok: fixture text
            git("add", "."); git("commit", "-qm", "type-change")
            old_root = lint.ROOT; lint.ROOT = repo
            try:
                added = lint.added_lines("HEAD~1", "HEAD")
            finally:
                lint.ROOT = old_root
            self.assertIn("tools/test_a.py", added)
            self.assertEqual(len(lint.scan_lines("tools/test_a.py", added["tools/test_a.py"], NO_BASELINE)), 1)

    def test_form_feed_inside_added_line_is_scanned(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp); git = lambda *a: subprocess.run(["git", "-C", tmp, *a], check=True, capture_output=True)
            git("init", "-q", "-b", "main"); git("config", "user.email", "t@example.com"); git("config", "user.name", "t")
            (repo / "tools").mkdir(); f = repo / "tools" / "test_a.py"; f.write_text("old = 1\n", encoding="utf-8")
            git("add", "."); git("commit", "-qm", "base")
            f.write_bytes(b"old = 1\nvalue = 0\fvalue = time.time()\n"); git("add", "."); git("commit", "-qam", "plant")  # clock-ok: fixture text
            old_root = lint.ROOT; lint.ROOT = repo
            try: added = lint.added_lines("HEAD~1", "HEAD")
            finally: lint.ROOT = old_root
            self.assertEqual(len(lint.scan_lines("tools/test_a.py", added["tools/test_a.py"], NO_BASELINE)), 1)

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

    def test_emit_baseline_covers_multiline_clock_span(self):
        with tempfile.TemporaryDirectory() as tmp:
            repo = Path(tmp)
            git = lambda *a: subprocess.run(["git", "-C", tmp, *a], check=True, capture_output=True)
            git("init", "-q", "-b", "main")
            git("config", "user.email", "t@example.com")
            git("config", "user.name", "t")
            (repo / "tools" / "scripts").mkdir(parents=True)
            (repo / "tools" / "test_multiline.py").write_text(
                "value = (\n    time\n    .time()\n)\n", encoding="utf-8")
            (repo / "tools" / "scripts" / "clock_in_tests_lint.py").write_text(
                (HERE / "clock_in_tests_lint.py").read_text(encoding="utf-8"),
                encoding="utf-8")
            git("add", ".")
            git("commit", "-qm", "base")
            emitted = subprocess.run(
                [sys.executable, str(repo / "tools/scripts/clock_in_tests_lint.py"), "--emit-baseline"],
                cwd=repo, capture_output=True, text=True, encoding="utf-8")
            self.assertEqual(emitted.returncode, 0, emitted.stdout + emitted.stderr)
            baseline = repo / "tools/scripts/clock_in_tests_baseline.json"
            baseline.write_text(emitted.stdout, encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(repo / "tools/scripts/clock_in_tests_lint.py"), "--all"],
                cwd=repo, capture_output=True, text=True, encoding="utf-8")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
