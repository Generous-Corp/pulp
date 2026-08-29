#!/usr/bin/env python3
"""Mutation tests for the terminal A2T acceptance verifier."""

from __future__ import annotations

import copy
import importlib.util
import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = Path(__file__).with_name("verify_gpu_trace_overhead_acceptance.py")
SPEC = importlib.util.spec_from_file_location("verify_gpu_trace_overhead_acceptance", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
CONTRACT = MODULE.contract


class VerifyGpuTraceOverheadAcceptanceTests(unittest.TestCase):
    def terminal_receipt(self):
        head = CONTRACT._git_text(ROOT, "rev-parse", "HEAD")
        trace = ROOT / "test/fixtures/perfetto-gpu/first-frame-pipeline-upload-stall.pftrace"
        binding = CONTRACT.source_binding(ROOT, head, trace)
        evidence_id = "44444444444444444444444444444444"
        semantic_base = {
            "schema": "pulp.trace-gpu-analysis.v1",
            "capture_complete": True,
            "observed_categories": ["gpu"], "category_scope": None,
            "contributors": [], "cold_start_contributors": [],
            "steady_state_contributors": [], "scheduler_evidence_available": False,
            "capture_integrity": {"slice_count": 1}, "evidence_ids": [],
            "next_actions": [], "ui_correlation": {},
        }
        startup = {
            **semantic_base,
            "question": "gpu-startup", "verdict": "unverified",
            "dominant_stage": "pipeline-prepare", "contributors": [
                {"rank": 1, "stage": "pipeline-prepare", "duration_ns": 1_800_000,
                 "evidence_id": evidence_id, "frame_index": 0, "sequence": 1,
                 "health_state": "healthy"},
                {"rank": 2, "stage": "resource-upload", "duration_ns": 900_000,
                 "evidence_id": evidence_id, "frame_index": 0, "sequence": 2,
                 "health_state": "healthy"},
            ], "evidence_ids": [evidence_id],
            "next_actions": [{"code": "inspect-pipeline-signature"}],
        }
        replay = []
        for case, filename, question, verdict, dominant, action in CONTRACT.FIXTURE_REPLAY:
            fixture = ROOT / "test/fixtures/perfetto-gpu" / filename
            semantic = copy.deepcopy(startup) if filename == trace.name else {
                **semantic_base,
                "question": question,
                "verdict": verdict,
                "dominant_stage": dominant,
                "next_actions": ([{"code": action}] if action else []),
            }
            replay.append({
                "case": case,
                "question": question,
                "trace": {
                    "role": f"repository/test/fixtures/perfetto-gpu/{filename}",
                    "sha256": CONTRACT.sha256(fixture),
                    "bytes": fixture.stat().st_size,
                },
                "cli_rerun": "pass",
                "cli_mcp_parity": "pass",
                "semantic_result": semantic,
            })
        trace_digest = CONTRACT.sha256(trace)
        measured_cli = [1] * 30
        measured_mcp = [1] * 30
        fresh_cli = [1] * 20
        fresh_mcp = [1] * 20
        return {
            "schema": "pulp.gpu-trace-overhead-acceptance.v2",
            "source_revision": head,
            "mcp_source_revision": head,
            "integration_head": binding["integration_head"],
            "source_blobs": binding["source_blobs"],
            "source_identity": {
                "repository": "Generous-Corp/pulp", "revision": head,
                "clean": True,
            },
            "installed_source_identity": {
                "source_revision": head,
                "build_info_sha256": "1" * 64,
                "build_info": {
                    "kBuildType": "Release", "kGitDirty": False,
                    "kGitSha": head[:12], "kSdkVersion": "0.999.0",
                },
            },
            "accepted_plan": {
                "repository": "danielraffel/pulp-planning",
                "revision": MODULE.EXPECTED_PLAN_REVISION,
                "path": CONTRACT.PLAN_PATH,
                "blob": "2d1c461d3ea640f75786a72c312d074f68f59028",
                "sha256": MODULE.EXPECTED_PLAN_SHA256,
            },
            "artifacts": {
                "sibling_binding": {"verified_same_resolved_parent": True},
                "cli": {"sha256": "2" * 64, "bytes": 1},
                "mcp": {"sha256": "3" * 64, "bytes": 1},
                "trace": {
                    "role": "repository/test/fixtures/perfetto-gpu/" + trace.name,
                    "sha256": trace_digest, "bytes": trace.stat().st_size,
                },
                "trace_processor": {
                    "version": "v57.2", "platform": "mac-arm64",
                    "sha256": CONTRACT.PROCESSOR_SHA256["mac-arm64"],
                    "version_output": "Perfetto v57.2-da1d152cf",
                },
            },
            "protocol": {
                "question": "gpu-startup", "warmups": 5,
                "measured_paired_trials": 30, "fresh_start_paired_trials": 20,
                "order": "alternating cli-first/mcp-first",
                "environment_path": "/usr/bin:/bin:/usr/sbin:/sbin",
            },
            "measured": {
                "cli": CONTRACT.summary(measured_cli),
                "persistent_mcp_request": CONTRACT.summary(measured_mcp),
                "confidence": CONTRACT.paired_delta_confidence(
                    measured_cli, measured_mcp
                ),
                "raw_samples": [
                {"trial": i + 1, "order": "cli-first" if i % 2 == 0 else "mcp-first",
                 "cli_duration_ns": 1, "mcp_duration_ns": 1}
                for i in range(30)
            ]},
            "fresh_start": {"cli": CONTRACT.summary(fresh_cli),
                "mcp_process_initialize_request_shutdown": CONTRACT.summary(fresh_mcp),
                "raw_samples": [
                {"trial": i + 1, "order": "cli-first" if i % 2 == 0 else "mcp-first",
                 "cli_duration_ns": 1,
                 "mcp_process_initialize_request_shutdown_duration_ns": 1}
                for i in range(20)
            ]},
            "fixture_replay": replay,
            "semantic_result": startup,
            "human_perfetto_ui_correlation": {
                "artifact_sha256": trace_digest,
                "reviewer": "independent human reviewer",
                "reviewed_utc": "2026-08-29T00:00:00Z",
                "ui_revision": "v58.3-example",
                "delivery": "official localhost embedding protocol; localOnly",
                "observed_spans": [
                    {"name": "gpu_pipeline_prepare", "duration_ns": 1_800_000,
                     "gpu_evidence_id": evidence_id, "frame_index": 0,
                     "sequence": 1, "health_state": "healthy"},
                    {"name": "gpu_resource_upload", "duration_ns": 900_000,
                     "gpu_evidence_id": evidence_id, "frame_index": 0,
                     "sequence": 2, "health_state": "healthy"},
                ],
            },
            "acceptance": {
                "terminal_status": "pass", "semantic_parity": "pass",
                "same_installed_prefix": "pass",
                "human_perfetto_ui_correlation": "pass",
            },
        }

    def test_terminal_exact_head_receipt_passes(self):
        self.assertEqual(MODULE.verify(self.terminal_receipt(), ROOT), [])

    def test_nonterminal_and_old_receipts_fail_closed(self):
        receipt = self.terminal_receipt()
        receipt["acceptance"]["terminal_status"] = "nonterminal"
        self.assertIn("receipt is nonterminal", MODULE.verify(receipt, ROOT))
        old = json.loads((ROOT / "docs/validation/gpu-trace-overhead/"
                          "m3-a2t-offline-analysis-20260828.json").read_text())
        errors = MODULE.verify(old, ROOT)
        self.assertTrue(any("receipt schema" in error for error in errors))

    def test_processor_plan_and_fixture_mutations_fail_closed(self):
        mutations = [
            (lambda r: r["artifacts"]["trace_processor"].update(sha256="0" * 64),
             "processor digest"),
            (lambda r: r["accepted_plan"].update(revision="0" * 40),
             "accepted plan revision"),
            (lambda r: r["fixture_replay"].pop(), "fixture replay"),
        ]
        for mutate, expected in mutations:
            with self.subTest(expected=expected):
                receipt = self.terminal_receipt()
                mutate(receipt)
                self.assertTrue(any(expected in error for error in MODULE.verify(receipt, ROOT)))

    def test_human_observation_must_match_current_semantics(self):
        receipt = self.terminal_receipt()
        receipt["human_perfetto_ui_correlation"]["observed_spans"][0]["duration_ns"] = 1
        errors = MODULE.verify(receipt, ROOT)
        self.assertTrue(any("human Perfetto UI correlation" in error for error in errors))

    def test_sample_count_and_order_mutations_fail_closed(self):
        receipt = self.terminal_receipt()
        receipt["measured"]["raw_samples"].pop()
        receipt["fresh_start"]["raw_samples"][1]["order"] = "cli-first"
        errors = MODULE.verify(receipt, ROOT)
        self.assertIn("measured raw sample count must be exactly 30", errors)
        self.assertTrue(any("fresh trial 2" in error for error in errors))

    def test_published_statistics_must_match_raw_samples(self):
        receipt = self.terminal_receipt()
        receipt["measured"]["cli"]["median_ms"] = 999.0
        receipt["measured"]["confidence"]["ci_high_ms"] = 999.0
        receipt["fresh_start"]["mcp_process_initialize_request_shutdown"][
            "p95_ms"
        ] = 999.0
        errors = MODULE.verify(receipt, ROOT)
        self.assertIn("measured CLI summary does not match raw durations", errors)
        self.assertIn("measured confidence interval does not match raw durations", errors)
        self.assertIn("fresh MCP summary does not match raw durations", errors)


if __name__ == "__main__":
    unittest.main()
