#!/usr/bin/env python3
"""Tests for cleanup artifact deletion helpers."""

from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("cleanup_plan_apply.py", module_name="pulp_cleanup_plan_apply")


class CleanupPlanApplyTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_apply_cleanup_plan_removes_files_and_directories(self) -> None:
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            bundle = root / "old.bundle"
            bundle.write_text("old")
            log_dir = root / "old"
            log_dir.mkdir()
            (log_dir / "mac.log").write_text("old")
            plan = {
                "categories": {
                    "bundles": [{"path": bundle, "size_bytes": 3}],
                    "logs": [{"path": log_dir, "size_bytes": 3}],
                }
            }

            result = self.mod.apply_local_ci_cleanup_plan(plan)

            self.assertFalse(bundle.exists())
            self.assertFalse(log_dir.exists())
            self.assertEqual(len(result["removed"]), 2)
            self.assertEqual(result["removed_bytes"], 6)
            self.assertEqual(result["failed"], [])


if __name__ == "__main__":
    unittest.main()
