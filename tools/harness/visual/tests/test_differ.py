"""Tests for the semantic visual snapshot differ."""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parents[3]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tools.harness.visual import differ  # noqa: E402


class DifferTests(unittest.TestCase):
    def test_numeric_tolerance_accepts_small_rect_deltas(self) -> None:
        expected = {"nodes": [{"rect": {"x": 10.0, "y": 20.0, "w": 30.0, "h": 40.0}}]}
        actual = {"nodes": [{"rect": {"x": 10.0004, "y": 20.0, "w": 30.0, "h": 40.0}}]}

        self.assertEqual(differ.compare(expected, actual, tolerance=0.001), [])

    def test_numeric_tolerance_reports_large_deltas(self) -> None:
        expected = {"nodes": [{"rect": {"x": 10.0}}]}
        actual = {"nodes": [{"rect": {"x": 10.01}}]}

        diffs = differ.compare(expected, actual, tolerance=0.001)

        self.assertEqual(len(diffs), 1)
        self.assertEqual(diffs[0].path, "$.nodes[0].rect.x")

    def test_missing_key_is_reported(self) -> None:
        diffs = differ.compare({"a": 1, "b": 2}, {"a": 1})

        self.assertEqual(len(diffs), 1)
        self.assertEqual(diffs[0].path, "$.b")
        self.assertEqual(diffs[0].message, "missing key")

    def test_extra_key_array_length_bool_and_value_mismatches_are_reported(self) -> None:
        diffs = differ.compare(
            {"enabled": True, "items": ["a", "b"], "mode": "fit"},
            {"enabled": False, "items": ["a"], "mode": "fill", "extra": 3},
        )

        messages = {(d.path, d.message) for d in diffs}
        self.assertIn(("$.enabled", "boolean mismatch"), messages)
        self.assertIn(("$.items", "array length mismatch"), messages)
        self.assertIn(("$.mode", "value mismatch"), messages)
        self.assertIn(("$.extra", "unexpected key"), messages)

    def test_type_changed_number_to_bool_is_not_treated_as_numeric(self) -> None:
        diffs = differ.compare({"opacity": 1}, {"opacity": True})

        self.assertEqual(len(diffs), 1)
        self.assertEqual(diffs[0].path, "$.opacity")
        self.assertEqual(diffs[0].message, "boolean mismatch")

    def test_matching_booleans_do_not_report_differences(self) -> None:
        self.assertEqual(differ.compare({"enabled": True}, {"enabled": True}), [])

    def test_format_differences_limits_output_and_formats_empty_list(self) -> None:
        diffs = [
            differ.Difference(f"$.items[{i}]", i, i + 1, "value mismatch")
            for i in range(3)
        ]

        self.assertEqual(differ.format_differences([]), "")
        text = differ.format_differences(diffs, limit=2)
        self.assertIn("$.items[0]: value mismatch", text)
        self.assertIn("... 1 more difference(s)", text)
        self.assertNotIn("$.items[2]", text)

    def test_load_json_and_diff_files_use_file_contents(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            expected = root / "expected.json"
            actual = root / "actual.json"
            expected.write_text('{"rect":{"x":1.0}}', encoding="utf-8")
            actual.write_text('{"rect":{"x":1.5}}', encoding="utf-8")

            self.assertEqual(differ.load_json(expected), {"rect": {"x": 1.0}})
            diffs = differ.diff_files(expected, actual, tolerance=0.1)

        self.assertEqual(len(diffs), 1)
        self.assertEqual(diffs[0].path, "$.rect.x")

    def test_tolerance_from_fixture_prefers_rect_value(self) -> None:
        self.assertEqual(
            differ.tolerance_from_fixture({"tolerance": {"rect": 0.25}}),
            0.25,
        )

    def test_tolerance_from_fixture_precedence_and_invalid_values(self) -> None:
        self.assertEqual(differ.tolerance_from_fixture(None, default=0.5), 0.5)
        self.assertEqual(differ.tolerance_from_fixture({"tolerance": "loose"}), 0.001)
        self.assertEqual(
            differ.tolerance_from_fixture({"tolerance": {"semantic": "0.125", "rect": 1}}),
            0.125,
        )
        self.assertEqual(
            differ.tolerance_from_fixture({"tolerance": {"number": "bad"}}, default=0.25),
            0.25,
        )

    def test_byte_difference_summarizes_exact_mismatch(self) -> None:
        self.assertEqual(differ.format_byte_difference(b"abc", b"abc"), "")
        self.assertIn(
            "byte mismatch at offset 1",
            differ.format_byte_difference(b"abc", b"axc"),
        )
        self.assertEqual(
            differ.format_byte_difference(b"abc", b"abcd"),
            "length mismatch: expected_len=3 actual_len=4",
        )


if __name__ == "__main__":
    unittest.main()
