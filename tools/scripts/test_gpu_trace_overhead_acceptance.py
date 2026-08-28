#!/usr/bin/env python3

import importlib.util
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


SCRIPT = Path(__file__).with_name("gpu_trace_overhead_acceptance.py")
SPEC = importlib.util.spec_from_file_location("gpu_trace_overhead_acceptance", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class GpuTraceOverheadAcceptanceTests(unittest.TestCase):
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
            result = MODULE.commit_inventory(Path("/repo"), "revision")
        self.assertFalse(result["no_added_producer_call_sites"])
        self.assertEqual(result["added_or_changed_producer_paths"],
                         ["core/render/src/new_trace.cpp"])


if __name__ == "__main__":
    unittest.main()
