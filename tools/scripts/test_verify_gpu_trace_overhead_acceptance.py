#!/usr/bin/env python3
"""Mutation tests for A2T structural verification boundaries."""

from __future__ import annotations

import copy
import importlib.util
import json
import tempfile
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

    def structural_receipt(self):
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
        human = None
        human_source = None
        provider_tree = {
            "method": "retained-complete-tree-vnode-and-descriptor-v1",
            "file_count": 3,
            "bytes": 3,
            "manifest_sha256": "7" * 64,
        }
        manifest_sha256, skia_assets = CONTRACT._pinned_provider_asset_digests(
            "Skia"
        )
        _manifest_sha256, v8_assets = CONTRACT._pinned_provider_asset_digests("V8")
        skia_asset = sorted(skia_assets)[0]
        v8_asset = sorted(v8_assets)[0]
        skia_stamp_sha256 = CONTRACT.hashlib.sha256(
            (skia_asset + "\n").encode()
        ).hexdigest()
        v8_stamp_sha256 = CONTRACT.hashlib.sha256(
            (v8_asset + "\n").encode()
        ).hexdigest()
        providers = {
            "skia_dawn": {
                "root_role": "resolved-skia-dawn-provider-root",
                "resolution": "exact CMake/Ninja Skia layout plus pinned asset generation",
                "root_authority": {
                    "method": "pinned-release-asset-stamp-and-exact-layout-v1",
                    "dependency": "Skia",
                    "dependency_manifest_path": CONTRACT.PROVIDER_MANIFEST_PATH,
                    "dependency_manifest_sha256": manifest_sha256,
                    "generation_stamp_path": ".skia-asset-sha256",
                    "generation_asset_sha256": skia_asset,
                    "generation_stamp_sha256": skia_stamp_sha256,
                    "top_level_entries": [
                        ".skia-asset-sha256", "build", "include"
                    ],
                    "cache_authority": "SKIA_DIR",
                    "provider_layout": "build/<platform>-gpu/lib/Release",
                    "consumed_skia_archives": [
                        "build/mac-gpu/lib/Release/libskia.a"
                    ],
                    "consumed_top_level_entries": ["build"],
                },
                "tree_claim": provider_tree,
                "required_members": [
                    {"path": ".skia-asset-sha256", "git_blob_sha1": "0" * 40,
                     "sha256": skia_stamp_sha256, "bytes": 65},
                    {"path": "lib/libskia.a", "git_blob_sha1": "1" * 40,
                     "sha256": "1" * 64, "bytes": 1},
                    {"path": "lib/libdawn_combined.a", "git_blob_sha1": "2" * 40,
                     "sha256": "2" * 64, "bytes": 1},
                ],
            },
            "v8": {
                "root_role": "resolved-v8-provider-root",
                "resolution": "exact CMake V8 runtime layout plus pinned asset generation",
                "root_authority": {
                    "method": "pinned-release-asset-stamp-and-exact-layout-v1",
                    "dependency": "V8",
                    "dependency_manifest_path": CONTRACT.PROVIDER_MANIFEST_PATH,
                    "dependency_manifest_sha256": manifest_sha256,
                    "generation_stamp_path": ".v8-asset-sha256",
                    "generation_asset_sha256": v8_asset,
                    "generation_stamp_sha256": v8_stamp_sha256,
                    "top_level_entries": [".v8-asset-sha256", "include", "lib"],
                    "cache_authority": "V8_RUNTIME_LIBRARY",
                    "provider_layout": "lib/<runtime>",
                    "consumed_runtime": "lib/libv8.dylib",
                },
                "tree_claim": provider_tree,
                "required_members": [
                    {"path": ".v8-asset-sha256", "git_blob_sha1": "5" * 40,
                     "sha256": v8_stamp_sha256, "bytes": 65},
                    {"path": "include/v8.h", "git_blob_sha1": "3" * 40,
                     "sha256": "3" * 64, "bytes": 1},
                    {"path": "lib/libv8.dylib", "git_blob_sha1": "4" * 40,
                     "sha256": "4" * 64, "bytes": 1},
                ],
            },
        }
        source_disposition = "not-present-at-configured-or-resolved-sibling"
        provider_claim = {
            "method": "retained-resolved-render-provider-trees-v3",
            "skia_source_disposition": source_disposition,
            "providers": providers,
            "manifest_sha256": CONTRACT.hashlib.sha256(
                json.dumps(
                    {
                        "providers": providers,
                        "skia_source_disposition": source_disposition,
                    },
                    sort_keys=True,
                    separators=(",", ":"),
                ).encode()
            ).hexdigest(),
        }
        return {
            "schema": "pulp.gpu-trace-overhead-acceptance.v3",
            "source_revision": head,
            "mcp_source_revision": head,
            "integration_head": binding["integration_head"],
            "source_blobs": binding["source_blobs"],
            "executing_source_identity": CONTRACT.executing_checkout_source_identity(
                ROOT, head
            ),
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
                "terminal_status": CONTRACT.OFFLINE_STRUCTURAL_STATUS,
                "semantic_parity": "pass",
                "same_installed_prefix": "pass",
                "human_perfetto_ui_correlation": (
                    "unverified-no-human-perfetto-ui-correlation"
                ),
                "offline_latency_budget": "unverified-no-ratified-budget",
                "producer_overhead_budget": "not-applicable-no-a2t-scoped-producer-delta",
                "xrun_check": "not-applicable-offline-no-audio-thread",
            },
        }

    @staticmethod
    def attach_historical_agent_review(receipt):
        relative = (
            "docs/validation/gpu-trace-overhead/"
            "m3-a2t-offline-analysis-20260828.json"
        )
        data = (ROOT / relative).read_bytes()
        prior = json.loads(data)
        head = receipt["source_revision"]
        receipt["human_perfetto_ui_correlation"] = copy.deepcopy(
            prior["human_perfetto_ui_correlation"]
        )
        receipt["human_perfetto_ui_review_source"] = {
            "method": "preexisting-tracked-git-blob-v1",
            "repository_path": relative,
            "source_revision": head,
            "git_blob_sha1": CONTRACT._git_text(
                ROOT, "rev-parse", f"{head}:{relative}"
            ),
            "sha256": CONTRACT.hashlib.sha256(data).hexdigest(),
            "bytes": len(data),
            "existed_unchanged_in_direct_parent": True,
        }
        receipt["acceptance"]["human_perfetto_ui_correlation"] = "pass"

    def test_synthetic_exact_head_receipt_passes_structural_validation_only(self):
        self.assertEqual(MODULE.verify(self.structural_receipt(), ROOT), [])

    def test_synthetic_receipt_cannot_request_terminal_certification(self):
        receipt = self.structural_receipt()
        errors = MODULE.verify(receipt, ROOT, require_terminal=True)
        self.assertTrue(any(
            "standalone A2T verification and recording are structural-only" in error
            for error in errors
        ))

    def test_terminal_claim_and_old_receipts_fail_closed(self):
        receipt = self.structural_receipt()
        receipt["acceptance"]["terminal_status"] = "pass"
        self.assertTrue(any(
            "terminal_status differs" in error for error in MODULE.verify(receipt, ROOT)
        ))
        old = json.loads((ROOT / "docs/validation/gpu-trace-overhead/"
                          "m3-a2t-offline-analysis-20260828.json").read_text())
        errors = MODULE.verify(old, ROOT)
        self.assertTrue(any("receipt schema" in error for error in errors))

    def test_stale_parent_cannot_request_terminal_certification(self):
        receipt = self.structural_receipt()
        parent = CONTRACT._git_text(ROOT, "rev-parse", "HEAD^")
        receipt["source_revision"] = parent
        receipt["mcp_source_revision"] = parent
        receipt["integration_head"] = parent
        errors = MODULE.verify(receipt, ROOT, require_terminal=True)
        self.assertTrue(any(
            "to equal live checkout HEAD" in error for error in errors
        ))

    def test_verifier_rejects_separate_clean_clone_at_older_sha(self):
        receipt = self.structural_receipt()
        with tempfile.TemporaryDirectory() as temporary:
            clone = Path(temporary) / "older-clean-clone"
            CONTRACT.subprocess.run(
                ["git", "clone", "--quiet", "--no-hardlinks", str(ROOT), str(clone)],
                check=True,
            )
            CONTRACT.subprocess.run(
                ["git", "checkout", "--quiet", "HEAD^"], cwd=clone, check=True
            )
            errors = MODULE.verify(receipt, clone)
        self.assertTrue(any(
            "executing A2T source checkout is invalid" in error
            and "exact checkout containing the executing recorder" in error
            for error in errors
        ), errors)

    def test_verifier_rejects_forged_executing_source_identity(self):
        receipt = self.structural_receipt()
        receipt["executing_source_identity"]["source_blobs"][
            "tools/scripts/sdk_provenance.py"
        ] = "0" * 40
        self.assertIn(
            "executing_source_identity does not bind this exact recorder/verifier checkout",
            MODULE.verify(receipt, ROOT),
        )

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
                receipt = self.structural_receipt()
                mutate(receipt)
                self.assertTrue(any(expected in error for error in MODULE.verify(receipt, ROOT)))

    def test_empty_installed_source_stamp_fails_closed(self):
        receipt = self.structural_receipt()
        receipt["installed_source_identity"]["build_info"]["kGitSha"] = ""
        errors = MODULE.verify(receipt, ROOT)
        self.assertIn("installed build stamp is not bound to source_revision", errors)

    def test_mixed_installed_binary_and_build_output_fails_closed(self):
        receipt = self.structural_receipt()
        receipt["installed_source_identity"]["build_provenance"]["binaries"][
            "pulp-mcp"
        ]["build_output_sha256"] = "6" * 64
        errors = MODULE.verify(receipt, ROOT)
        self.assertIn(
            "installed pulp-mcp is not byte-identical to its exact build output",
            errors,
        )

        receipt = self.structural_receipt()
        receipt["artifacts"]["cli"]["sha256"] = "7" * 64
        errors = MODULE.verify(receipt, ROOT)
        self.assertIn("measured cli artifact differs from build provenance", errors)

    def test_build_output_bytes_are_required_typed_and_equal(self):
        for value in (None, False, 2):
            with self.subTest(value=value):
                receipt = self.structural_receipt()
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
        receipt = self.structural_receipt()
        del receipt["installed_source_identity"]["build_provenance"][
            "source_tree_claim"
        ]
        errors = MODULE.verify(receipt, ROOT)
        self.assertTrue(any("retained exact Git source" in error for error in errors))

        receipt = self.structural_receipt()
        receipt["installed_source_identity"]["build_provenance"][
            "build_input_claim"
        ]["build_targets"] = []
        errors = MODULE.verify(receipt, ROOT)
        self.assertTrue(any("forced-clean build inputs" in error for error in errors))

        receipt = self.structural_receipt()
        receipt["installed_source_identity"]["build_provenance"][
            "render_provider_input_claim"
        ]["providers"]["v8"]["required_members"].pop()
        errors = MODULE.verify(receipt, ROOT)
        self.assertTrue(any("V8 provider" in error for error in errors))

    def test_consumed_adjacent_skia_source_provider_cannot_be_omitted(self):
        receipt = self.structural_receipt()
        claim = receipt["installed_source_identity"]["build_provenance"][
            "render_provider_input_claim"
        ]
        claim["skia_source_disposition"] = (
            "retained-complete-adjacent-source-tree"
        )
        claim["manifest_sha256"] = CONTRACT.hashlib.sha256(
            json.dumps(
                {
                    "providers": claim["providers"],
                    "skia_source_disposition": claim[
                        "skia_source_disposition"
                    ],
                },
                sort_keys=True,
                separators=(",", ":"),
            ).encode()
        ).hexdigest()
        errors = MODULE.verify(receipt, ROOT)
        self.assertTrue(any("sealed render providers" in error for error in errors))

    def test_render_provider_root_provenance_cannot_be_forged(self):
        receipt = self.structural_receipt()
        claim = receipt["installed_source_identity"]["build_provenance"][
            "render_provider_input_claim"
        ]
        claim["providers"]["skia_dawn"]["root_role"] = "caller-selected-root"
        claim["manifest_sha256"] = CONTRACT.hashlib.sha256(
            json.dumps(
                {
                    "providers": claim["providers"],
                    "skia_source_disposition": claim[
                        "skia_source_disposition"
                    ],
                },
                sort_keys=True,
                separators=(",", ":"),
            ).encode()
        ).hexdigest()
        errors = MODULE.verify(receipt, ROOT)
        self.assertTrue(any("exact root provenance" in error for error in errors))

    def test_render_provider_generation_rejects_unrelated_monorepo_entry(self):
        receipt = self.structural_receipt()
        claim = receipt["installed_source_identity"]["build_provenance"][
            "render_provider_input_claim"
        ]
        claim["providers"]["skia_dawn"]["root_authority"][
            "top_level_entries"
        ].append("unrelated-product-gpu")
        claim["manifest_sha256"] = CONTRACT.hashlib.sha256(
            json.dumps(
                {
                    "providers": claim["providers"],
                    "skia_source_disposition": claim[
                        "skia_source_disposition"
                    ],
                },
                sort_keys=True,
                separators=(",", ":"),
            ).encode()
        ).hexdigest()
        errors = MODULE.verify(receipt, ROOT)
        self.assertTrue(any("pinned generation authority" in error for error in errors))

    def test_no_producer_disposition_is_recomputed_from_git(self):
        receipt = self.structural_receipt()
        inventory = receipt["producer_overhead_disposition"]["evidence"]
        inventory["path_deltas"][0]["source_blob"] = "0" * 40
        errors = MODULE.verify(receipt, ROOT)
        self.assertTrue(any("path-scoped tree delta" in error for error in errors))

    def test_no_producer_inventory_cannot_omit_real_a2t_behavior_delta(self):
        receipt = self.structural_receipt()
        deltas = receipt["producer_overhead_disposition"]["evidence"]["path_deltas"]
        target = "experimental/pulp-rs/src/cmd/trace_gpu_analysis.rs"
        deltas[:] = [row for row in deltas if row["path"] != target]
        errors = MODULE.verify(receipt, ROOT)
        self.assertTrue(any("complete path-scoped tree delta" in error for error in errors))

    def test_later_non_a2t_product_producers_cannot_be_hidden(self):
        receipt = self.structural_receipt()
        receipt["producer_overhead_disposition"]["evidence"][
            "non_a2t_product_producers"
        ] = []
        errors = MODULE.verify(receipt, ROOT)
        self.assertTrue(any("every post-base non-A2T product producer" in error for error in errors))

    def test_fixture_semantic_question_cannot_be_relabelled(self):
        receipt = self.structural_receipt()
        receipt["fixture_replay"][0]["semantic_result"]["question"] = "gpu-probe"
        errors = MODULE.verify(receipt, ROOT)
        self.assertTrue(any("semantic result has the wrong question" in error for error in errors))

    def test_historical_agent_review_cannot_satisfy_human_acceptance(self):
        receipt = self.structural_receipt()
        self.attach_historical_agent_review(receipt)
        errors = MODULE.verify(receipt, ROOT)
        self.assertTrue(any(
            "unsafe repository path" in error
            or "structural review evidence" in error
            for error in errors
        ))

    def test_human_review_cannot_self_authenticate(self):
        receipt = self.structural_receipt()
        self.attach_historical_agent_review(receipt)
        del receipt["human_perfetto_ui_review_source"]
        errors = MODULE.verify(receipt, ROOT)
        self.assertTrue(any(
            "human Perfetto UI correlation" in error
            or "structural review evidence" in error
            for error in errors
        ))

        receipt = self.structural_receipt()
        self.attach_historical_agent_review(receipt)
        receipt["human_perfetto_ui_review_source"]["git_blob_sha1"] = "0" * 40
        errors = MODULE.verify(receipt, ROOT)
        self.assertTrue(any(
            "unsafe repository path" in error
            or "structural review evidence" in error
            for error in errors
        ))

    def test_sample_count_and_order_mutations_fail_closed(self):
        receipt = self.structural_receipt()
        receipt["measured"]["raw_samples"].pop()
        receipt["fresh_start"]["raw_samples"][1]["order"] = "cli-first"
        errors = MODULE.verify(receipt, ROOT)
        self.assertIn("measured raw sample count must be exactly 30", errors)
        self.assertTrue(any("fresh trial 2" in error for error in errors))

    def test_published_statistics_must_match_raw_samples(self):
        receipt = self.structural_receipt()
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
