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
    def human_review_receipt(self, trace_digest="1" * 64):
        return {
            "protocol": {"question": "gpu-startup"},
            "acceptance": {"human_perfetto_ui_correlation": "pass"},
            "artifacts": {"trace": {"sha256": trace_digest}},
            "human_perfetto_ui_correlation": {
                "artifact_sha256": trace_digest,
                "reviewer": "human reviewer",
                "reviewed_utc": "2026-08-28T05:34:54Z",
                "ui_revision": "v58.3-11fbaed8",
                "delivery": "official localhost embedding protocol",
                "observed_spans": [{"name": "gpu_pipeline_prepare"}],
            },
        }

    def test_preserves_exact_passing_human_review_for_same_startup_trace(self):
        receipt = self.human_review_receipt()
        result = MODULE.preserve_human_perfetto_ui_correlation(
            receipt, question="gpu-startup", trace_sha256="1" * 64
        )
        self.assertEqual(result, receipt["human_perfetto_ui_correlation"])

    def test_human_review_rejects_different_trace_digest(self):
        with self.assertRaisesRegex(ValueError, "not bound to the measured trace"):
            MODULE.preserve_human_perfetto_ui_correlation(
                self.human_review_receipt(),
                question="gpu-startup", trace_sha256="2" * 64,
            )

    def test_human_review_rejects_mismatched_prior_trace_artifact(self):
        receipt = self.human_review_receipt()
        receipt["artifacts"]["trace"]["sha256"] = "2" * 64
        with self.assertRaisesRegex(ValueError, "not bound to its receipt trace"):
            MODULE.preserve_human_perfetto_ui_correlation(
                receipt, question="gpu-startup", trace_sha256="1" * 64
            )

    def test_human_review_rejects_non_startup_question(self):
        with self.assertRaisesRegex(ValueError, "only to gpu-startup"):
            MODULE.preserve_human_perfetto_ui_correlation(
                self.human_review_receipt(),
                question="gpu-health", trace_sha256="1" * 64,
            )

    def test_human_review_rejects_missing_acceptance_or_root_object(self):
        missing_acceptance = self.human_review_receipt()
        del missing_acceptance["acceptance"]
        with self.assertRaisesRegex(ValueError, "lacks passing"):
            MODULE.preserve_human_perfetto_ui_correlation(
                missing_acceptance, question="gpu-startup", trace_sha256="1" * 64
            )
        missing_root = self.human_review_receipt()
        del missing_root["human_perfetto_ui_correlation"]
        with self.assertRaisesRegex(ValueError, "lacks the root correlation object"):
            MODULE.preserve_human_perfetto_ui_correlation(
                missing_root, question="gpu-startup", trace_sha256="1" * 64
            )

    def test_human_review_rejects_non_passing_prior_acceptance(self):
        receipt = self.human_review_receipt()
        receipt["acceptance"]["human_perfetto_ui_correlation"] = "unverified"
        with self.assertRaisesRegex(ValueError, "lacks passing"):
            MODULE.preserve_human_perfetto_ui_correlation(
                receipt, question="gpu-startup", trace_sha256="1" * 64
            )

    def test_human_review_rejects_stripped_visual_review_fields(self):
        for field in ("reviewer", "reviewed_utc", "ui_revision", "delivery"):
            with self.subTest(field=field):
                receipt = self.human_review_receipt()
                del receipt["human_perfetto_ui_correlation"][field]
                with self.assertRaisesRegex(ValueError, f"lacks nonempty {field}"):
                    MODULE.preserve_human_perfetto_ui_correlation(
                        receipt, question="gpu-startup", trace_sha256="1" * 64
                    )
        receipt = self.human_review_receipt()
        receipt["human_perfetto_ui_correlation"]["observed_spans"] = []
        with self.assertRaisesRegex(ValueError, "lacks observed span details"):
            MODULE.preserve_human_perfetto_ui_correlation(
                receipt, question="gpu-startup", trace_sha256="1" * 64
            )

    def test_human_review_rejects_prior_non_startup_receipt(self):
        receipt = self.human_review_receipt()
        receipt["protocol"]["question"] = "gpu-health"
        with self.assertRaisesRegex(ValueError, "must be for gpu-startup"):
            MODULE.preserve_human_perfetto_ui_correlation(
                receipt, question="gpu-startup", trace_sha256="1" * 64
            )

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

    def test_checked_in_receipt_preserves_a3_human_review_binding(self):
        receipt = json.loads(
            (ROOT / "docs" / "validation" / "gpu-trace-overhead" /
             "m3-a2t-offline-analysis-20260828.json").read_text(encoding="utf-8")
        )
        trace_digest = receipt["artifacts"]["trace"]["sha256"]
        correlation = MODULE.preserve_human_perfetto_ui_correlation(
            receipt, question=receipt["protocol"]["question"],
            trace_sha256=trace_digest,
        )
        self.assertEqual(receipt["protocol"]["question"], "gpu-startup")
        self.assertEqual(
            receipt["acceptance"]["human_perfetto_ui_correlation"], "pass"
        )
        self.assertEqual(correlation, receipt["human_perfetto_ui_correlation"])
        self.assertEqual(correlation["artifact_sha256"], trace_digest)

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
