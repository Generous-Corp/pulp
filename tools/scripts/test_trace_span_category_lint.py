#!/usr/bin/env python3
"""Tests for trace_span_category_lint.py.

The load-bearing half is `KnownPositivesTest`: a lint whose real value is a
clean run has to prove, against a fixture holding a counted set of known
violations, that it finds exactly that many. A green sweep over the tree is
otherwise indistinguishable from a lint that matches nothing at all.
"""

from __future__ import annotations

import importlib.util
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location(
    "trace_span_category_lint", HERE / "trace_span_category_lint.py")
lint = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(lint)


def scan(text: str, suffix: str = ".cpp") -> list[tuple]:
    """Run the lint over a one-file tree holding `text`."""
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / f"source{suffix}").write_text(text, encoding="utf-8")
        return lint.violations(root)


class DefectShapeTest(unittest.TestCase):
    """The macOS window-host defect the lint exists to prevent."""

    def test_gpu_name_under_render_is_flagged(self):
        found = scan('  PULP_TRACE_SCOPE_NAMED("render", "gpu_submit");\n')
        self.assertEqual(len(found), 1)
        _path, lineno, macro, category, name = found[0]
        self.assertEqual((lineno, macro, category, name),
                         (1, "SCOPE_NAMED", "render", "gpu_submit"))

    def test_all_three_window_host_spans_are_flagged(self):
        self.assertEqual(len(scan(
            '  PULP_TRACE_SCOPE_NAMED("render", "gpu_acquire");\n'
            '  PULP_TRACE_SCOPE_NAMED("render", "gpu_submit");\n'
            '  PULP_TRACE_SCOPE_NAMED("render", "gpu_present");\n')), 3)

    def test_gpu_category_is_accepted(self):
        self.assertEqual(scan('  PULP_TRACE_SCOPE_NAMED("gpu", "gpu_submit");\n'), [])

    def test_gpu_prefixed_category_is_accepted(self):
        """The SQL layer selects `category GLOB 'gpu*'`, so a sub-category
        such as `gpu_timing` is visible to it and must not be flagged."""
        self.assertEqual(scan('  PULP_TRACE_SCOPE_NAMED("gpu_timing", "gpu_submit");\n'), [])


class DiscriminationTest(unittest.TestCase):
    """The lint keys on the NAME prefix, not on the category alone."""

    def test_non_gpu_name_under_render_is_untouched(self):
        self.assertEqual(scan(
            '  PULP_TRACE_SCOPE_NAMED("render", "window_resize");\n'
            '  PULP_TRACE_SCOPE_NAMED("render", "skia_begin");\n'
            '  PULP_TRACE_SCOPE_NAMED("render", "frame");\n'), [])

    def test_gpu_substring_that_is_not_a_prefix_is_untouched(self):
        self.assertEqual(scan('  PULP_TRACE_SCOPE_NAMED("render", "begin_gpu_work");\n'), [])

    def test_macro_definition_is_untouched(self):
        """The definition takes unquoted identifiers, so requiring literal
        string arguments skips it without an explicit exception."""
        self.assertEqual(scan(
            "#define PULP_TRACE_SCOPE_NAMED(category, name) ((void)0)\n"), [])


class MacroFormTest(unittest.TestCase):
    """Every emitting macro form that takes (category, name) literals."""

    def test_scope_named_args(self):
        self.assertEqual(len(scan(
            '  PULP_TRACE_SCOPE_NAMED_ARGS("render", "gpu_submit", "index", i);\n')), 1)

    def test_counter(self):
        self.assertEqual(len(scan(
            '  PULP_TRACE_COUNTER("render", "gpu_render_time_ms", ms);\n')), 1)

    def test_begin_and_end(self):
        self.assertEqual(len(scan(
            '  PULP_TRACE_BEGIN("render", "gpu_present");\n'
            '  PULP_TRACE_BEGIN_ARGS("render", "gpu_acquire", "id", id);\n')), 2)

    def test_longer_macro_name_wins_the_alternation(self):
        """SCOPE_NAMED_ARGS must not be reported as SCOPE_NAMED."""
        found = scan('  PULP_TRACE_SCOPE_NAMED_ARGS("render", "gpu_submit", "k", v);\n')
        self.assertEqual(found[0][2], "SCOPE_NAMED_ARGS")


class ExemptionTest(unittest.TestCase):
    def test_skip_marker_suppresses(self):
        self.assertEqual(scan(
            '  PULP_TRACE_SCOPE_NAMED("render", "gpu_submit");  '
            '// trace-span-category-lint: skip deliberate\n'), [])

    def test_commented_out_emitter_is_ignored(self):
        self.assertEqual(scan(
            '  // PULP_TRACE_SCOPE_NAMED("render", "gpu_submit");\n'
            '   * PULP_TRACE_SCOPE_NAMED("render", "gpu_present");\n'), [])

    def test_non_source_suffix_is_not_scanned(self):
        self.assertEqual(scan(
            '  PULP_TRACE_SCOPE_NAMED("render", "gpu_submit");\n', suffix=".md"), [])

    def test_objective_cpp_is_scanned(self):
        """The defect lived in a .mm file; excluding that suffix would make
        the whole lint vacuous."""
        self.assertEqual(len(scan(
            '  PULP_TRACE_SCOPE_NAMED("render", "gpu_submit");\n', suffix=".mm")), 1)


class KnownPositivesTest(unittest.TestCase):
    """Sensitivity against a counted fixture, across files and suffixes.

    A lint that reports zero over the real tree proves nothing unless it is
    known to find violations it is shown. This fixture carries 6 planted
    violations among 6 correct emitters; anything but 6 means the instrument
    is reading less than it appears to.
    """

    VIOLATIONS = [
        ('  PULP_TRACE_SCOPE_NAMED("render", "gpu_acquire");\n', ".mm"),
        ('  PULP_TRACE_SCOPE_NAMED("render", "gpu_submit");\n', ".mm"),
        ('  PULP_TRACE_SCOPE_NAMED("render", "gpu_present");\n', ".mm"),
        ('  PULP_TRACE_SCOPE_NAMED_ARGS("canvas", "gpu_readback", "n", n);\n', ".cpp"),
        ('  PULP_TRACE_COUNTER("render", "gpu_render_time_ms", ms);\n', ".cpp"),
        ('  PULP_TRACE_BEGIN("layout", "gpu_device_init");\n', ".hpp"),
    ]
    CLEAN = [
        ('  PULP_TRACE_SCOPE_NAMED("gpu", "gpu_submit");\n', ".cpp"),
        ('  PULP_TRACE_SCOPE_NAMED("gpu", "gpu_present");\n', ".cpp"),
        ('  PULP_TRACE_SCOPE_NAMED("gpu", "gpu_acquire");\n', ".mm"),
        ('  PULP_TRACE_SCOPE_NAMED("render", "window_resize");\n', ".mm"),
        ('  PULP_TRACE_SCOPE_NAMED("render", "skia_begin");\n', ".cpp"),
        ('  PULP_TRACE_SCOPE_NAMED("layout", "layout_children");\n', ".hpp"),
    ]

    def _build(self, root: Path) -> None:
        for index, (text, suffix) in enumerate(self.VIOLATIONS):
            (root / f"bad_{index}{suffix}").write_text(text, encoding="utf-8")
        for index, (text, suffix) in enumerate(self.CLEAN):
            (root / f"good_{index}{suffix}").write_text(text, encoding="utf-8")

    def test_finds_every_planted_violation_and_no_others(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._build(root)
            found = lint.violations(root)
        self.assertEqual(len(found), len(self.VIOLATIONS))
        self.assertEqual(
            sorted(name for _p, _l, _m, _c, name in found),
            sorted(["gpu_acquire", "gpu_submit", "gpu_present",
                    "gpu_readback", "gpu_render_time_ms", "gpu_device_init"]))

    def test_clean_half_alone_reports_nothing(self):
        """The control for the test above: without the planted violations the
        same fixture must go quiet, or the count proves nothing."""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for index, (text, suffix) in enumerate(self.CLEAN):
                (root / f"good_{index}{suffix}").write_text(text, encoding="utf-8")
            self.assertEqual(lint.violations(root), [])

    def test_nested_directories_are_reached(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            deep = root / "core" / "view" / "platform" / "mac"
            deep.mkdir(parents=True)
            (deep / "window_host_mac.mm").write_text(
                '  PULP_TRACE_SCOPE_NAMED("render", "gpu_submit");\n', encoding="utf-8")
            self.assertEqual(len(lint.violations(root)), 1)


class ExitCodeTest(unittest.TestCase):
    def test_main_returns_zero_on_a_clean_tree(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "a.cpp").write_text(
                '  PULP_TRACE_SCOPE_NAMED("gpu", "gpu_submit");\n', encoding="utf-8")
            self.assertEqual(lint.main(["--root", str(root)]), 0)

    def test_main_returns_one_on_a_violation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "a.cpp").write_text(
                '  PULP_TRACE_SCOPE_NAMED("render", "gpu_submit");\n', encoding="utf-8")
            self.assertEqual(lint.main(["--root", str(root)]), 1)


if __name__ == "__main__":
    unittest.main()
