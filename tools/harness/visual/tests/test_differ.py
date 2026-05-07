"""Tests for the semantic visual snapshot differ."""

from __future__ import annotations

import sys
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

    def test_tolerance_from_fixture_prefers_rect_value(self) -> None:
        self.assertEqual(
            differ.tolerance_from_fixture({"tolerance": {"rect": 0.25}}),
            0.25,
        )


if __name__ == "__main__":
    unittest.main()
