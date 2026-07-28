#!/usr/bin/env python3
"""Unit tests for designated_initializer_lint.py."""
from __future__ import annotations

import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from designated_initializer_lint import scan_text  # noqa: E402


class ScanTextTests(unittest.TestCase):
    def test_clean_initializer_has_no_findings(self):
        src = """
Binders b{
    .gain_for = [] { return nullptr; },
    .plugin_for = [] { return nullptr; },
    .custom_latency_for = [] { return 0; },
};
"""
        self.assertEqual(scan_text(src, "x.cpp"), [])

    def test_duplicate_designator_is_flagged(self):
        # The exact shape a merge produced in core/host/src/signal_graph.cpp:
        # a byte-identical block re-appended at the end of the initializer.
        src = """
Binders b{
    .gain_for = [] { return nullptr; },
    .custom_latency_for = [] { return 0; },
    .plugin_latency_for = [] { return 0; },
    .custom_latency_for = [] { return 0; },
};
"""
        findings = scan_text(src, "signal_graph.cpp")
        self.assertEqual(len(findings), 1)
        self.assertIn("custom_latency_for", findings[0])
        self.assertIn("signal_graph.cpp:6", findings[0])
        # Points back at the first one so the reader can compare the two.
        self.assertIn("line 4", findings[0])

    def test_sibling_aggregates_may_reuse_a_name(self):
        # Two separate structs each naming `.width` is legal and common.
        src = """
Rect a{ .width = 1, .height = 2 };
Rect b{ .width = 3, .height = 4 };
"""
        self.assertEqual(scan_text(src, "x.cpp"), [])

    def test_nested_aggregate_may_reuse_the_outer_name(self):
        # The depth counter is what makes this pass; a flat name set would
        # produce a false positive on ordinary nested initialization.
        src = """
Outer o{
    .value = 1,
    .inner = Inner{
        .value = 2,
    },
};
"""
        self.assertEqual(scan_text(src, "x.cpp"), [])

    def test_repeat_inside_one_nested_aggregate_is_still_flagged(self):
        src = """
Outer o{
    .inner = Inner{
        .value = 1,
        .other = 2,
        .value = 3,
    },
};
"""
        findings = scan_text(src, "x.cpp")
        self.assertEqual(len(findings), 1)
        self.assertIn(".value", findings[0])

    def test_member_assignment_is_not_a_designator(self):
        # `obj.field = ...` mid-function must not be read as a designator, or
        # any function that assigns the same field twice would be flagged.
        src = """
void f() {
    obj.field = 1;
    obj.field = 2;
}
"""
        self.assertEqual(scan_text(src, "x.cpp"), [])

    def test_wrapped_comparison_is_not_a_designator(self):
        # Catch2 assertions wrap like this all over the repo; `.status ==`
        # starts the line, and a bare `=` match would flag every repetition.
        src = """
void f() {
    REQUIRE(evaluate(Factors{2.0}, 4.0)
                .status == Status::Allowed);
    REQUIRE(evaluate(Factors{3.0}, 4.0)
                .status == Status::Rejected);
}
"""
        self.assertEqual(scan_text(src, "x.cpp"), [])

    def test_compound_assignment_is_still_a_designator_free_zone(self):
        src = """
void f() {
    total += 1;
    total += 2;
}
"""
        self.assertEqual(scan_text(src, "x.cpp"), [])

    def test_wrapped_member_assignment_is_not_a_designator(self):
        # A long assignment wraps so `.field =` starts the line, which is
        # indistinguishable from a designator without looking at what precedes
        # it. Repeated in test_sample_heritage_json.cpp and initially flagged.
        src = """
void f() {
    std::get<Block>(changed.voice[6].parameters)
        .stopband_attenuation_db = 72.0f;
    std::get<Block>(changed.record[1].parameters)
        .stopband_attenuation_db = 72.0f;
}
"""
        self.assertEqual(scan_text(src, "x.cpp"), [])

    def test_braces_in_comments_do_not_move_depth(self):
        # A stray `}` in prose would otherwise close the aggregate early and
        # hide a real duplicate behind it.
        src = """
Binders b{
    .a = 1,
    // a closing brace } in prose
    .a = 2,
};
"""
        findings = scan_text(src, "x.cpp")
        self.assertEqual(len(findings), 1)

    def test_braces_in_string_literals_do_not_move_depth(self):
        src = """
Binders b{
    .a = "{{{",
    .a = 2,
};
"""
        findings = scan_text(src, "x.cpp")
        self.assertEqual(len(findings), 1)

    def test_skip_marker_suppresses_a_finding(self):
        src = """
Binders b{
    .a = 1,
    .a = 2,  // designated-init-lint: skip
};
"""
        self.assertEqual(scan_text(src, "x.cpp"), [])


class RepositoryTests(unittest.TestCase):
    def test_signal_graph_is_clean(self):
        """The file this lint was written for must stay fixed."""
        root = pathlib.Path(__file__).resolve().parents[2]
        target = root / "core" / "host" / "src" / "signal_graph.cpp"
        if not target.exists():
            self.skipTest("signal_graph.cpp not present in this checkout")
        findings = scan_text(target.read_text(encoding="utf-8",
                                              errors="replace"), str(target))
        self.assertEqual(findings, [], "\n".join(findings))


if __name__ == "__main__":
    unittest.main()
