#!/usr/bin/env python3
"""Schema checks for the clock/date guard data files.

This test deliberately validates only the data contract. The consuming guards
land in the follow-up rule PR.
"""

from __future__ import annotations

import json
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class ClockGuardDataTest(unittest.TestCase):
    def test_clock_baseline_schema(self):
        data = json.loads((ROOT / "tools/scripts/clock_in_tests_baseline.json").read_text(encoding="utf-8"))
        self.assertEqual(set(data), {"files", "lines"})
        self.assertIsInstance(data["files"], list)
        self.assertIsInstance(data["lines"], list)
        for entry in data["lines"]:
            self.assertEqual(set(entry), {"path", "line", "reason"})
            self.assertTrue(entry["path"].startswith("tools/"))
            self.assertTrue(entry["line"])
            self.assertTrue(entry["reason"])

    def test_date_matrix_schema(self):
        data = json.loads((ROOT / "tools/scripts/date_sensitive_selftests.json").read_text(encoding="utf-8"))
        self.assertEqual(set(data), {"_comment", "zones", "tests", "pending"})
        self.assertEqual(set(data["zones"]), {"Pacific/Kiritimati", "Pacific/Pago_Pago", "UTC"})
        for section in ("tests", "pending"):
            for entry in data[section]:
                self.assertEqual(set(entry), {"path", "reason"})
                self.assertTrue(entry["path"].startswith("tools/"))
                self.assertTrue(entry["reason"])


if __name__ == "__main__":
    unittest.main()
