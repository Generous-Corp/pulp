#!/usr/bin/env python3
"""Tests for desktop proof and run-summary helpers."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("reporting_proofs.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class ReportingProofsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_source_mode_and_manifest_fallback_edges(self) -> None:
        self.assertEqual(self.mod.normalize_desktop_proof_source_mode(None), "legacy")
        self.assertEqual(self.mod.normalize_desktop_proof_source_mode(" exact_sha "), "exact-sha")
        with self.assertRaisesRegex(ValueError, "Invalid desktop proof source mode"):
            self.mod.normalize_desktop_proof_source_mode("archive")

        config = {
            "desktop_automation": {
                "targets": {
                    "mac": {"adapter": "macos-local"},
                }
            }
        }
        self.assertEqual(self.mod.desktop_manifest_adapter(config, {"adapter": "custom"}), "custom")
        self.assertEqual(self.mod.desktop_manifest_adapter(config, {"target": "mac"}), "macos-local")
        self.assertEqual(self.mod.desktop_manifest_adapter(config, {"target": "missing"}), "unknown")
        self.assertEqual(self.mod.desktop_manifest_adapter({"desktop_automation": {"targets": []}}, {"target": "mac"}), "unknown")
        self.assertEqual(self.mod.desktop_manifest_source({})["mode"], "legacy")
        self.assertEqual(self.mod.desktop_manifest_source({"source": {"mode": "bad"}})["mode"], "legacy")

    def test_run_summary_and_proof_summaries_filter_latest_passing_runs(self) -> None:
        config = {
            "desktop_automation": {
                "targets": {
                    "mac": {"adapter": "macos-local"},
                    "linux": {"adapter": "linux-xvfb"},
                }
            }
        }
        manifests = [
            {
                "target": "mac",
                "action": "smoke",
                "label": "new",
                "completed_at": "2026-05-22T13:00:00+00:00",
                "status": "error",
                "source": {"mode": "exact-sha", "sha": "b" * 40},
            },
            {
                "target": "mac",
                "action": "smoke",
                "label": "old",
                "completed_at": "2026-05-22T12:00:00+00:00",
                "status": "pass",
                "source": {"mode": "exact-sha", "sha": "a" * 40},
                "artifacts": {"screenshot": "after.png"},
            },
            {
                "target": "mac",
                "action": "smoke",
                "label": "older",
                "completed_at": "2026-05-22T11:00:00+00:00",
                "status": "pass",
                "source": {"mode": "exact-sha", "sha": "a" * 40},
            },
        ]

        summary = self.mod.desktop_run_summary(config, manifests[1])
        self.assertEqual(summary["adapter"], "macos-local")
        self.assertEqual(summary["proof_scope"], "local-session")
        self.assertEqual(summary["run_status"], "pass")
        self.assertEqual(summary["artifacts"]["screenshot"], "after.png")

        proofs = self.mod.desktop_proof_summaries(
            config,
            target_name="mac",
            action="smoke",
            source_mode="exact-sha",
            desktop_run_manifests_fn=lambda _config, **_kwargs: manifests,
            desktop_run_summary_fn=self.mod.desktop_run_summary,
        )
        self.assertEqual(len(proofs), 1)
        self.assertEqual(proofs[0]["latest_run"]["label"], "old")
        self.assertEqual(proofs[0]["run_count"], 2)


if __name__ == "__main__":
    unittest.main()
