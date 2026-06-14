#!/usr/bin/env python3
"""Tests for split desktop proof helper modules."""

from __future__ import annotations

from pathlib import Path
import unittest

from module_test_utils import load_module_from_path


def load_module(name: str):
    return load_module_from_path(Path(__file__).with_name(f"{name}.py"), add_module_dir=True)


class ReportingProofModuleTests(unittest.TestCase):
    def test_source_and_run_summary_helpers_cover_manifest_fallbacks(self) -> None:
        source = load_module("reporting_proof_source")
        summary = load_module("reporting_run_summary")
        config = {"desktop_automation": {"targets": {"mac": {"adapter": "macos-local"}}}}
        manifest = {
            "target": "mac",
            "action": "smoke",
            "label": "run",
            "status": "pass",
            "source": {"mode": "exact_sha", "sha": "a" * 40},
            "artifacts": {"screenshot": "after.png"},
        }

        self.assertEqual(source.normalize_desktop_proof_source_mode(" exact_sha "), "exact-sha")
        with self.assertRaisesRegex(ValueError, "Invalid desktop proof source mode"):
            source.normalize_desktop_proof_source_mode("archive")
        self.assertEqual(source.desktop_manifest_source({"source": {"mode": "bad"}})["mode"], "legacy")

        run_summary = summary.desktop_run_summary(config, manifest)
        self.assertEqual(summary.desktop_manifest_adapter(config, {"target": "missing"}), "unknown")
        self.assertEqual(summary.desktop_manifest_run_status({}), "pass")
        self.assertEqual(summary.desktop_proof_scope_for_adapter("linux-xvfb"), "live-host")
        self.assertEqual(run_summary["adapter"], "macos-local")
        self.assertEqual(run_summary["source"]["mode"], "exact-sha")
        self.assertEqual(run_summary["artifacts"]["screenshot"], "after.png")

    def test_proof_list_aggregates_latest_passing_runs(self) -> None:
        proof_list = load_module("reporting_proof_list")
        summary = load_module("reporting_run_summary")
        config = {"desktop_automation": {"targets": {"mac": {"adapter": "macos-local"}}}}
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

        proofs = proof_list.desktop_proof_summaries(
            config,
            target_name="mac",
            action="smoke",
            source_mode="exact-sha",
            desktop_run_manifests_fn=lambda _config, **_kwargs: manifests,
            desktop_run_summary_fn=summary.desktop_run_summary,
        )

        self.assertEqual(len(proofs), 1)
        self.assertEqual(proofs[0]["latest_run"]["label"], "old")
        self.assertEqual(proofs[0]["run_count"], 2)


if __name__ == "__main__":
    unittest.main()
