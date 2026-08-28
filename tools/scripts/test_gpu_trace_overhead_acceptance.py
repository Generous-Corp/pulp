#!/usr/bin/env python3

import importlib.util
import json
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


SCRIPT = Path(__file__).with_name("gpu_trace_overhead_acceptance.py")
ROOT = SCRIPT.resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("gpu_trace_overhead_acceptance", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class GpuTraceOverheadAcceptanceTests(unittest.TestCase):
    def test_plan_binding_requires_exact_lowercase_hex(self):
        self.assertTrue(MODULE.valid_lower_hex("a" * 40, 40))
        self.assertTrue(MODULE.valid_lower_hex("0" * 64, 64))
        self.assertFalse(MODULE.valid_lower_hex("A" * 40, 40))
        self.assertFalse(MODULE.valid_lower_hex("a" * 39, 40))
        self.assertFalse(MODULE.valid_lower_hex("g" * 64, 64))

    def test_cli_and_mcp_must_share_one_source_revision(self):
        revision = "a" * 40
        self.assertFalse(MODULE.source_revisions_match(revision, ""))
        self.assertTrue(MODULE.source_revisions_match(revision, revision))
        self.assertFalse(MODULE.source_revisions_match(revision, "b" * 40))
        self.assertTrue(MODULE.valid_lower_hex(revision, 40))
        self.assertFalse(MODULE.valid_lower_hex("HEAD", 40))

    def test_checked_in_receipt_uses_one_source_checkout(self):
        receipt = json.loads(
            (ROOT / "docs" / "validation" / "gpu-trace-overhead" /
             "m3-a2t-offline-analysis-20260828.json").read_text(encoding="utf-8")
        )
        self.assertEqual(receipt["source_revision"], receipt["mcp_source_revision"])
        self.assertTrue(receipt["artifacts"]["sibling_binding"]
                        ["verified_same_resolved_parent"])
        disposition = receipt["producer_overhead_disposition"]
        self.assertEqual(disposition["formal_plan_status"], "accepted-canonical-plan")
        self.assertTrue(MODULE.valid_lower_hex(disposition["formal_plan_revision"], 40))
        self.assertTrue(MODULE.valid_lower_hex(disposition["formal_plan_sha256"], 64))

    def test_percentile_interpolates(self):
        self.assertEqual(MODULE.percentile([1.0, 2.0, 3.0], 0.5), 2.0)
        self.assertAlmostEqual(MODULE.percentile([0.0, 10.0], 0.95), 9.5)

    def test_summary_retains_tail_and_noise(self):
        result = MODULE.summary([1_000_000, 2_000_000, 3_000_000])
        self.assertEqual(result["count"], 3)
        self.assertEqual(result["median_ms"], 2.0)
        self.assertEqual(result["p95_ms"], 2.9)
        self.assertEqual(result["mad_ms"], 1.0)

    def test_bootstrap_is_deterministic_and_paired(self):
        first = MODULE.bootstrap_median_delta_ci([1, 2, 3], [11, 12, 13])
        second = MODULE.bootstrap_median_delta_ci([1, 2, 3], [11, 12, 13])
        self.assertEqual(first, second)
        self.assertEqual(first["mcp_minus_cli_median_ms"], 0.00001)

    def test_semantic_projection_excludes_transport_only_fields(self):
        payload = {"schema": "pulp.trace-gpu-analysis.v1", "question": "gpu-health",
                   "verdict": "pass", "capture_complete": True,
                   "dominant_stage": "health-transition", "transport_debug": "not semantic"}
        self.assertNotIn("transport_debug", MODULE.semantic_projection(payload))

    def test_parse_analysis_rejects_unknown_verdict(self):
        with self.assertRaises(RuntimeError):
            MODULE.parse_analysis(
                {"schema": "pulp.trace-gpu-analysis.v1", "verdict": "ok"}, surface="test"
            )

    def test_commit_inventory_detects_a_producer_path(self):
        completed = SimpleNamespace(
            returncode=0,
            stdout="tools/mcp/mcp_trace_tools.cpp\ncore/render/src/new_trace.cpp\n",
            stderr="",
        )
        with mock.patch.object(MODULE.subprocess, "run", return_value=completed):
            result = MODULE.commit_inventory(Path("/repo"), "a" * 40)
        self.assertFalse(result["no_added_producer_call_sites"])
        self.assertEqual(result["added_or_changed_producer_paths"],
                         ["core/render/src/new_trace.cpp"])

    def test_commit_inventory_rejects_moving_ref(self):
        with self.assertRaises(ValueError):
            MODULE.commit_inventory(Path("/repo"), "HEAD")


if __name__ == "__main__":
    unittest.main()
