#!/usr/bin/env python3
"""Tests for date_sensitive_selftests_tz_matrix.py."""

from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location(
    "tz_matrix", HERE / "date_sensitive_selftests_tz_matrix.py")
matrix = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = matrix
SPEC.loader.exec_module(matrix)

ZONES = ["Pacific/Kiritimati", "Pacific/Pago_Pago", "UTC"]

# Compares the local calendar date with the UTC one, as a fixture built from the
# local clock beside a UTC-pinned checker does. The zones differ from UTC by a
# day at every moment for at least one of them, so this exits non-zero somewhere.
CLOCK_DEPENDENT = (
    "import datetime as d, sys\n"
    "sys.exit(0 if d.date.today() == d.datetime.now(d.timezone.utc).date() else 1)\n"
)
PINNED = (
    "import datetime as d, sys\n"
    "sys.exit(0 if d.date(2026, 11, 12) < d.date(2026, 11, 19) else 1)\n"
)


class MatrixTest(unittest.TestCase):
    def _run(self, source: str, name: str) -> dict:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / name).write_text(source, encoding="utf-8")
            return matrix.run_matrix([name], ZONES, root)

    def test_clock_dependent_fixture_is_detected(self):
        results = self._run(CLOCK_DEPENDENT, "dep.py")
        self.assertTrue(matrix.failures(results), results)

    def test_pinned_fixture_passes_everywhere(self):
        results = self._run(PINNED, "pin.py")
        self.assertEqual(matrix.failures(results), [])
        self.assertEqual(set(results["pin.py"]), set(ZONES))

    def test_zone_reaches_the_child_process(self):
        probe = "import sys, time\nsys.exit(0 if time.tzname[0] == 'UTC' else 3)\n"
        results = self._run(probe, "tz.py")["tz.py"]
        self.assertEqual(results["UTC"], 0)
        self.assertNotEqual(results["Pacific/Kiritimati"], 0)

    def test_pending_entries_are_not_run_and_listed_tests_gate(self):
        with tempfile.TemporaryDirectory() as tmp:
            cfg = Path(tmp) / "c.json"
            cfg.write_text(json.dumps({
                "zones": ["UTC"], "tests": [],
                "pending": [{"path": "does/not/exist.py", "reason": "r"}]}), encoding="utf-8")
            self.assertEqual(matrix.main(["--config", str(cfg)]), 0)

    def test_shipped_config_is_well_formed(self):
        config = json.loads(matrix.CONFIG.read_text(encoding="utf-8"))
        self.assertEqual(config["zones"], ZONES)
        for entry in config["tests"] + config["pending"]:
            self.assertTrue((matrix.ROOT / entry["path"]).is_file(), entry)
            self.assertTrue(entry["reason"])


if __name__ == "__main__":
    unittest.main()
