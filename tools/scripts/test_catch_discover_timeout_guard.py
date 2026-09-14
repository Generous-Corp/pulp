#!/usr/bin/env python3
"""Self-tests for catch_discover_timeout_guard."""

from __future__ import annotations

import io
import sys
import contextlib
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import catch_discover_timeout_guard as guard  # noqa: E402


def tree(**manifests: str) -> Path:
    root = Path(tempfile.mkdtemp())
    (root / "test" / "cmake").mkdir(parents=True)
    for name, body in manifests.items():
        (root / "test" / "cmake" / f"{name}.cmake").write_text(body, encoding="utf-8")
    return root


def run(root: Path):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        rc = guard.main(["--root", str(root)])
    return rc, out.getvalue() + err.getvalue()


SCALED = """
pulp_scaled_test_timeout(_pulp_thing_timeout 300)
catch_discover_tests(pulp-test-thing
    PROPERTIES TIMEOUT "${_pulp_thing_timeout}")
"""

LITERAL = """
catch_discover_tests(pulp-test-thing
    PROPERTIES TIMEOUT 300)
"""


class ScanTests(unittest.TestCase):
    def test_scaled_budget_passes(self):
        rc, text = run(tree(a=SCALED))
        self.assertEqual(rc, 0, text)
        self.assertIn("0 literal TIMEOUT", text)

    def test_literal_budget_fails(self):
        rc, text = run(tree(a=LITERAL))
        self.assertEqual(rc, 1, text)
        self.assertIn("a.cmake:3", text)
        self.assertIn("TIMEOUT 300", text)

    def test_literal_on_the_call_line_is_caught(self):
        rc, _ = run(tree(a="catch_discover_tests(pulp-test-x PROPERTIES TIMEOUT 9)\n"))
        self.assertEqual(rc, 1)

    def test_a_scaled_sibling_does_not_excuse_a_literal(self):
        # The reviewer's finding on #8323: two registrations of the SAME binary
        # in the SAME file, one converted and one left as a literal. A
        # file-scoped check reads that file as converted.
        rc, text = run(tree(a=SCALED + LITERAL))
        self.assertEqual(rc, 1, text)
        self.assertEqual(text.count("TIMEOUT 300  ("), 1, text)

    def test_set_tests_properties_is_out_of_scope(self):
        body = 'set_tests_properties(pulp-test-thing PROPERTIES\n    TIMEOUT 1200)\n'
        rc, text = run(tree(a=SCALED + body))
        self.assertEqual(rc, 0, text)

    def test_commented_literal_is_ignored(self):
        body = """
catch_discover_tests(pulp-test-thing
    # a budget of TIMEOUT 300 was measured on a Release build
    PROPERTIES TIMEOUT "${_pulp_thing_timeout}")
"""
        rc, text = run(tree(a=body))
        self.assertEqual(rc, 0, text)

    def test_nested_parens_do_not_end_the_block_early(self):
        body = """
catch_discover_tests(pulp-test-thing
    TEST_SPEC "[a]$<IF:$<BOOL:1>,x,y>"
    PROPERTIES TIMEOUT 42)
"""
        rc, text = run(tree(a=body))
        self.assertEqual(rc, 1, text)
        self.assertIn("TIMEOUT 42", text)

    def test_skip_marker_excuses_a_line(self):
        body = """
catch_discover_tests(pulp-test-thing
    PROPERTIES TIMEOUT 300)  # catch-discover-timeout-guard: skip measured per-lane
"""
        rc, text = run(tree(a=body))
        self.assertEqual(rc, 0, text)

    def test_unbalanced_block_is_left_to_cmake(self):
        rc, text = run(tree(a=SCALED + "catch_discover_tests(pulp-test-y TIMEOUT 5\n"))
        self.assertEqual(rc, 0, text)

    def test_top_level_manifest_is_scanned(self):
        root = tree(a=SCALED)
        (root / "test" / "CMakeLists.txt").write_text(LITERAL, encoding="utf-8")
        rc, text = run(root)
        self.assertEqual(rc, 1, text)
        self.assertIn("CMakeLists.txt", text)


class InstrumentTests(unittest.TestCase):
    def test_a_tree_with_no_manifests_fails_rather_than_reporting_clean(self):
        # A zero finding over zero blocks is an instrument failure, not a pass.
        root = Path(tempfile.mkdtemp())
        (root / "test" / "cmake").mkdir(parents=True)
        rc, text = run(root)
        self.assertEqual(rc, 1, text)
        self.assertIn("parsed 0", text)

    def test_the_clean_report_states_its_control(self):
        rc, text = run(tree(a=SCALED, b=SCALED))
        self.assertEqual(rc, 0, text)
        self.assertIn("across 2 catch_discover_tests block(s)", text)

    def test_an_unknown_flag_exits_two_rather_than_scanning_nothing(self):
        with contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit) as caught:
                guard.main(["--no-such-flag"])
        self.assertEqual(caught.exception.code, 2)


class RepositoryTests(unittest.TestCase):
    def test_this_checkout_is_clean(self):
        root = Path(__file__).resolve().parents[2]
        rc, text = run(root)
        self.assertEqual(rc, 0, text)


if __name__ == "__main__":
    unittest.main()
