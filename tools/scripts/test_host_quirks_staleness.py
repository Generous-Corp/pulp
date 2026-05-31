#!/usr/bin/env python3
"""Unit tests for host_quirks_staleness.stale_entries (host-quirks P4).

Pure-logic tests with a fixed reference date — no clock, no network, no
catalog file needed. Run:
    python3 tools/scripts/test_host_quirks_staleness.py
"""

from __future__ import annotations

import datetime as _dt
import importlib.util
import pathlib
import unittest

_HERE = pathlib.Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location(
    "host_quirks_staleness", _HERE / "host_quirks_staleness.py")
hqs = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(hqs)


NOW = _dt.date(2026, 5, 30)


class StaleEntries(unittest.TestCase):
    def test_speculative_past_window_is_flagged(self) -> None:
        quirks = [{"flag": "old_spec", "tier": "Speculative",
                   "last_verified": "2025-09-01"}]  # ~9 months
        out = hqs.stale_entries(quirks, NOW, months=6)
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0]["_staleness"]["kind"], "age")
        self.assertGreaterEqual(out[0]["_staleness"]["age_months"], 6)

    def test_speculative_inside_window_is_not_flagged(self) -> None:
        quirks = [{"flag": "fresh_spec", "tier": "Speculative",
                   "last_verified": "2026-04-01"}]  # ~2 months
        self.assertEqual(hqs.stale_entries(quirks, NOW, months=6), [])

    def test_lesson_only_with_no_date_is_flagged(self) -> None:
        quirks = [{"flag": "dateless", "tier": "LessonOnly"}]
        out = hqs.stale_entries(quirks, NOW, months=6)
        self.assertEqual(len(out), 1)
        self.assertIsNone(out[0]["_staleness"]["age_months"])

    def test_validated_without_version_is_not_flagged(self) -> None:
        quirks = [{"flag": "plain_validated", "tier": "Validated",
                   "last_verified": "2024-01-01"}]
        # Validated + no affected_versions → no standing recheck prompt.
        self.assertEqual(hqs.stale_entries(quirks, NOW, months=6), [])

    def test_validated_with_version_gets_recheck_prompt(self) -> None:
        quirks = [{"flag": "ver_validated", "tier": "Validated",
                   "affected_versions": "13+", "last_verified": "2026-05-01"}]
        out = hqs.stale_entries(quirks, NOW, months=6)
        self.assertEqual(len(out), 1)
        self.assertEqual(out[0]["_staleness"]["kind"], "version-recheck")

    def test_months_between_boundary(self) -> None:
        # Exactly 6 months → flagged at the >= threshold.
        quirks = [{"flag": "exactly_six", "tier": "Speculative",
                   "last_verified": "2025-11-30"}]
        out = hqs.stale_entries(quirks, NOW, months=6)
        self.assertEqual(len(out), 1)


class RealCatalogSmoke(unittest.TestCase):
    """The shipped catalog runs through the detector without error."""

    def test_real_catalog_parses_and_runs(self) -> None:
        import json
        catalog = json.loads(hqs.DEFAULT_CATALOG.read_text(encoding="utf-8"))
        # Should not raise; returns a (possibly empty) list.
        out = hqs.stale_entries(catalog.get("quirks", []), NOW, months=6)
        self.assertIsInstance(out, list)


if __name__ == "__main__":
    unittest.main()
