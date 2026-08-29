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
    _scope_key: str | None = None
    _scope_inventory: dict | None = None

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
        if self.__class__._scope_key != head:
            self.__class__._scope_key = head
            self.__class__._scope_inventory = CONTRACT.a2t_scope_inventory(ROOT, head)
        scope_inventory = copy.deepcopy(self.__class__._scope_inventory)
        human, human_source = CONTRACT.bind_tracked_human_review_receipt(
            ROOT,
            head,
            ROOT / "docs/validation/gpu-trace-overhead/"
            "m3-a2t-offline-analysis-20260828.json",
            question="gpu-startup",
            trace_sha256=trace_digest,
            semantic_result=startup,
        )
        provider_tree = {
            "method": "retained-complete-tree-vnode-and-descriptor-v1",
            "file_count": 3,
            "bytes": 3,
            "manifest_sha256": "7" * 64,
        }
        providers = {
            "skia_dawn": {
                "root_role": "resolved-skia-dawn-provider-root",
                "resolution": "CMake SKIA_DIR or exact generated Ninja archive path",
                "tree_claim": provider_tree,
                "required_members": [
                    {"path": "lib/libskia.a", "git_blob_sha1": "1" * 40, "bytes": 1},
                    {"path": "lib/libdawn_combined.a", "git_blob_sha1": "2" * 40, "bytes": 1},
                ],
            },
            "v8": {
                "root_role": "resolved-v8-provider-root",
                "resolution": "CMake V8_RUNTIME_LIBRARY and enclosing include/v8.h root",
                "tree_claim": provider_tree,
                "required_members": [
                    {"path": "include/v8.h", "git_blob_sha1": "3" * 40, "bytes": 1},
                    {"path": "lib/libv8.dylib", "git_blob_sha1": "4" * 40, "bytes": 1},
                ],
            },
        }
        provider_claim = {
            "method": "retained-resolved-render-provider-trees-v1",
            "providers": providers,
            "manifest_sha256": CONTRACT.hashlib.sha256(
                json.dumps(providers, sort_keys=True, separators=(",", ":")).encode()
            ).hexdigest(),
        }
        return {
            "schema": "pulp.gpu-trace-overhead-acceptance.v3",
            "source_revision": head,
            "mcp_source_revision": head,
            "integration_head": binding["integration_head"],
            "source_blobs": binding["source_blobs"],
            "source_identity": {
                "repository": "Generous-Corp/pulp", "revision": head,
                "clean": True,
            },
            "installed_source_identity": {
                "prefix_role": "isolated-clean-release-install",
                "build_info_role": "installed-prefix/include/pulp/runtime/build_info.hpp",
                "source_revision": head,
                "build_info_sha256": "1" * 64,
                "build_info": {
                    "kBuildType": "Release", "kGitDirty": False,
                    "kGitSha": head[:12], "kSdkVersion": "0.999.0",
                },
                "build_provenance": {
                    "method": "fresh-external-cmake-build-install-byte-identity-v1",
                    "install_prefix_initial_state": "absent-and-atomically-claimed",
                    "install_prefix_claim_method": (
                        "unpredictable-staging-directory-renameatx-noreplace-retained-fd-v1"
                    ),
                    "install_prefix_claim": {"device": 1, "inode": 2},
                    "cmake_cache_sha256": "5" * 64,
                    "cmake_home_revision": head,
                    "build_targets": list(CONTRACT.BUILD_TARGETS),
                    "build_settings": dict(CONTRACT.REQUIRED_BUILD_SETTINGS),
                    "source_tree_claim": {
                        "method": "retained-git-tree-vnode-and-descriptor-v1",
                        "revision": head,
                        "root_tree": CONTRACT._git_text(
                            ROOT, "rev-parse", f"{head}^{{tree}}"
                        ),
                        "overlay_paths": [],
                        "excluded_gitlinks": MODULE._gitlinks(ROOT, head),
                        "regular_or_symlink_files": 1,
                        "retained_directories": 1,
                    },
                    "build_input_claim": {
                        "method": "regenerated-cmake-ninja-input-descriptor-v1",
                        "file_count": 3,
                        "manifest_sha256": "6" * 64,
                        "build_targets": list(CONTRACT.BUILD_TARGETS),
                        "forced_clean_before_build": True,
                    },
                    "render_provider_input_claim": provider_claim,
                    "binaries": {
                        "pulp": {
                            "installed_role": "installed-prefix/bin/pulp",
                            "build_output_role": "external-build/pulp",
                            "sha256": "2" * 64,
                            "build_output_sha256": "2" * 64,
                            "bytes": 1,
                            "build_output_bytes": 1,
                        },
                        "pulp-cpp": {
                            "installed_role": "installed-prefix/bin/pulp-cpp",
                            "build_output_role": "external-build/tools/cli/pulp-cpp",
                            "sha256": "4" * 64,
                            "build_output_sha256": "4" * 64,
                            "bytes": 1,
                            "build_output_bytes": 1,
                        },
                        "pulp-mcp": {
                            "installed_role": "installed-prefix/bin/pulp-mcp",
                            "build_output_role": "external-build/tools/mcp/pulp-mcp",
                            "sha256": "3" * 64,
                            "build_output_sha256": "3" * 64,
                            "bytes": 1,
                            "build_output_bytes": 1,
                        },
                    },
                },
            },
            "accepted_plan": {
                "repository": "danielraffel/pulp-planning",
                "revision": MODULE.EXPECTED_PLAN_REVISION,
                "path": CONTRACT.PLAN_PATH,
                "blob": "2d1c461d3ea640f75786a72c312d074f68f59028",
                "sha256": MODULE.EXPECTED_PLAN_SHA256,
            },
            "producer_overhead_disposition": {
                "status": "not-applicable-no-a2t-scoped-producer-cost",
                "non_a2t_owner_followup": (
                    "The input-to-present latency tracing and A3 packages must each provide or "
                    "bind tracing-off, tracing-on/idle, and active-capture overhead/control "
                    "evidence for their later product producers; A2T does not evaluate that "
                    "owner evidence."
                ),
                "required_followup": (
                    "B6 must run the three-state 5-warmup/30-trial and 20 "
                    "fresh-process protocol when Vellum producer instrumentation is added."
                ),
                "formal_plan_status": "accepted-canonical-plan",
                "formal_plan_revision": MODULE.EXPECTED_PLAN_REVISION,
                "formal_plan_sha256": MODULE.EXPECTED_PLAN_SHA256,
                "evidence": scope_inventory,
            },
            "artifacts": {
                "sibling_binding": {"verified_same_resolved_parent": True},
                "cli": {
                    "role": "installed-prefix/bin/pulp",
                    "sha256": "2" * 64,
                    "bytes": 1,
                },
                "mcp": {
                    "role": "installed-prefix/bin/pulp-mcp",
                    "sha256": "3" * 64,
                    "bytes": 1,
                },
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
            "human_perfetto_ui_correlation": human,
            "human_perfetto_ui_review_source": human_source,
            "acceptance": {
                "terminal_status": "pass", "semantic_parity": "pass",
                "same_installed_prefix": "pass",
                "human_perfetto_ui_correlation": "pass",
                "offline_latency_budget": "unverified-no-ratified-budget",
                "producer_overhead_budget": "not-applicable-no-a2t-scoped-producer-delta",
                "xrun_check": "not-applicable-offline-no-audio-thread",
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

    def test_empty_installed_source_stamp_fails_closed(self):
        receipt = self.terminal_receipt()
        receipt["installed_source_identity"]["build_info"]["kGitSha"] = ""
        errors = MODULE.verify(receipt, ROOT)
        self.assertIn("installed build stamp is not bound to source_revision", errors)

    def test_mixed_installed_binary_and_build_output_fails_closed(self):
        receipt = self.terminal_receipt()
        receipt["installed_source_identity"]["build_provenance"]["binaries"][
            "pulp-mcp"
        ]["build_output_sha256"] = "6" * 64
        errors = MODULE.verify(receipt, ROOT)
        self.assertIn(
            "installed pulp-mcp is not byte-identical to its exact build output",
            errors,
        )

        receipt = self.terminal_receipt()
        receipt["artifacts"]["cli"]["sha256"] = "7" * 64
        errors = MODULE.verify(receipt, ROOT)
        self.assertIn("measured cli artifact differs from build provenance", errors)

    def test_build_output_bytes_are_required_typed_and_equal(self):
        for value in (None, False, 2):
            with self.subTest(value=value):
                receipt = self.terminal_receipt()
                row = receipt["installed_source_identity"]["build_provenance"][
                    "binaries"
                ]["pulp-mcp"]
                if value is None:
                    del row["build_output_bytes"]
                else:
                    row["build_output_bytes"] = value
                errors = MODULE.verify(receipt, ROOT)
                self.assertIn(
                    "installed pulp-mcp is not byte-identical to its exact build output",
                    errors,
                )

    def test_source_and_forced_clean_build_claims_are_required(self):
        receipt = self.terminal_receipt()
        del receipt["installed_source_identity"]["build_provenance"][
            "source_tree_claim"
        ]
        errors = MODULE.verify(receipt, ROOT)
        self.assertTrue(any("retained exact Git source" in error for error in errors))

        receipt = self.terminal_receipt()
        receipt["installed_source_identity"]["build_provenance"][
            "build_input_claim"
        ]["build_targets"] = []
        errors = MODULE.verify(receipt, ROOT)
        self.assertTrue(any("forced-clean build inputs" in error for error in errors))

        receipt = self.terminal_receipt()
        receipt["installed_source_identity"]["build_provenance"][
            "render_provider_input_claim"
        ]["providers"]["v8"]["required_members"].pop()
        errors = MODULE.verify(receipt, ROOT)
        self.assertTrue(any("V8 provider" in error for error in errors))

    def test_no_producer_disposition_is_recomputed_from_git(self):
        receipt = self.terminal_receipt()
        inventory = receipt["producer_overhead_disposition"]["evidence"]
        inventory["path_deltas"][0]["source_blob"] = "0" * 40
        errors = MODULE.verify(receipt, ROOT)
        self.assertTrue(any("path-scoped tree delta" in error for error in errors))

    def test_no_producer_inventory_cannot_omit_real_a2t_behavior_delta(self):
        receipt = self.terminal_receipt()
        deltas = receipt["producer_overhead_disposition"]["evidence"]["path_deltas"]
        target = "experimental/pulp-rs/src/cmd/trace_gpu_analysis.rs"
        deltas[:] = [row for row in deltas if row["path"] != target]
        errors = MODULE.verify(receipt, ROOT)
        self.assertTrue(any("complete path-scoped tree delta" in error for error in errors))

    def test_later_non_a2t_product_producers_cannot_be_hidden(self):
        receipt = self.terminal_receipt()
        receipt["producer_overhead_disposition"]["evidence"][
            "non_a2t_product_producers"
        ] = []
        errors = MODULE.verify(receipt, ROOT)
        self.assertTrue(any("every later non-A2T product producer" in error for error in errors))

    def test_fixture_semantic_question_cannot_be_relabelled(self):
        receipt = self.terminal_receipt()
        receipt["fixture_replay"][0]["semantic_result"]["question"] = "gpu-probe"
        errors = MODULE.verify(receipt, ROOT)
        self.assertTrue(any("semantic result has the wrong question" in error for error in errors))

    def test_human_observation_must_match_current_semantics(self):
        receipt = self.terminal_receipt()
        receipt["human_perfetto_ui_correlation"]["observed_spans"][0]["duration_ns"] = 1
        errors = MODULE.verify(receipt, ROOT)
        self.assertTrue(any("human Perfetto UI correlation" in error for error in errors))

    def test_terminal_human_review_cannot_self_authenticate(self):
        receipt = self.terminal_receipt()
        del receipt["human_perfetto_ui_review_source"]
        errors = MODULE.verify(receipt, ROOT)
        self.assertTrue(any("human Perfetto UI correlation" in error for error in errors))

        receipt = self.terminal_receipt()
        receipt["human_perfetto_ui_review_source"]["git_blob_sha1"] = "0" * 40
        errors = MODULE.verify(receipt, ROOT)
        self.assertTrue(any("immutable Git provenance" in error for error in errors))

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
