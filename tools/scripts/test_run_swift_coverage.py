#!/usr/bin/env python3
"""Unit tests for tools/scripts/run_swift_coverage.py."""

from __future__ import annotations

import importlib.util
import pathlib
import sys
import unittest


SCRIPT = pathlib.Path(__file__).parent / "run_swift_coverage.py"

spec = importlib.util.spec_from_file_location("run_swift_coverage", SCRIPT)
assert spec and spec.loader
rsc = importlib.util.module_from_spec(spec)
sys.modules["run_swift_coverage"] = rsc
spec.loader.exec_module(rsc)


class ReportFilteringTests(unittest.TestCase):
    def test_source_entries_keep_only_repo_swift_sources(self) -> None:
        report = {
            "data": [
                {
                    "files": [
                        {
                            "filename": str(rsc.REPO_ROOT / "apple/Sources/PulpSwift/PulpBridge.swift"),
                            "summary": {"lines": {"covered": 10, "count": 12, "percent": 83.3}},
                        },
                        {
                            "filename": str(rsc.REPO_ROOT / "apple/Tests/PulpSwiftTests/PulpParameterTests.swift"),
                            "summary": {"lines": {"covered": 8, "count": 8, "percent": 100.0}},
                        },
                        {
                            "filename": str(rsc.REPO_ROOT / "apple/.build/debug/runner.swift"),
                            "summary": {"lines": {"covered": 1, "count": 1, "percent": 100.0}},
                        },
                    ]
                }
            ]
        }

        entries = rsc._source_entries(report)

        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0][0], "apple/Sources/PulpSwift/PulpBridge.swift")


class SummaryFormattingTests(unittest.TestCase):
    def test_format_summary_aggregates_source_line_totals(self) -> None:
        entries = [
            (
                "apple/Sources/PulpSwift/PulpBridge.swift",
                {"summary": {"lines": {"covered": 101, "count": 103, "percent": 98.0582}}},
            ),
            (
                "apple/Sources/PulpSwift/PulpViews.swift",
                {"summary": {"lines": {"covered": 262, "count": 298, "percent": 87.9194}}},
            ),
        ]

        text = rsc._format_summary(entries)

        self.assertIn("Files:       2", text)
        self.assertIn("Lines:       363/401 (90.52%)", text)
        self.assertIn("98.06%   101/103", text)
        self.assertIn("87.92%   262/298", text)

    def test_format_summary_rejects_empty_entries(self) -> None:
        with self.assertRaises(ValueError):
            rsc._format_summary([])


if __name__ == "__main__":
    unittest.main()
