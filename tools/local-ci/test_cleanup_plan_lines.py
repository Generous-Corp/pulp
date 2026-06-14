#!/usr/bin/env python3
"""Tests for cleanup plan line rendering."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("cleanup_plan_lines.py")


def load_module():
    return load_module_from_path(MODULE_PATH, module_name="pulp_cleanup_plan_lines")


class CleanupPlanLinesTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_cleanup_plan_lines_formats_summary_and_entries(self) -> None:
        entries = [{"path": Path(f"old-{idx}.bundle"), "size_bytes": idx + 1} for idx in range(12)]
        plan = {"categories": {"bundles": entries}, "total_bytes": 78, "total_paths": 12}

        lines = self.mod.cleanup_plan_lines(
            plan,
            dry_run=True,
            format_size_fn=lambda value: f"{value} B",
            describe_path_fn=lambda path: path.name,
        )

        self.assertEqual(lines[0:4], ["Local CI cleanup:", "", "  reclaimable: 78 B across 12 path(s)", ""])
        self.assertEqual(lines[4], "  bundles: 78 B across 12 path(s)")
        self.assertEqual(lines[15], "    ... 2 more")
        self.assertEqual(lines[-2:], ["", "  dry run only; re-run with --apply to delete these paths"])

        apply_lines = self.mod.cleanup_plan_lines(
            {"categories": {}, "total_bytes": 0, "total_paths": 0},
            dry_run=False,
            format_size_fn=lambda value: f"{value} B",
            describe_path_fn=lambda path: path.name,
        )
        self.assertEqual(apply_lines[-1], "  applying cleanup now")


if __name__ == "__main__":
    unittest.main()
