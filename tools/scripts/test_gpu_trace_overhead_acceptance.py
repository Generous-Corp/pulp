#!/usr/bin/env python3

import importlib.util
import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).with_name("gpu_trace_overhead_acceptance.py")
ROOT = SCRIPT.resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("gpu_trace_overhead_acceptance", SCRIPT)
assert SPEC and SPEC.loader
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class GpuTraceOverheadAcceptanceTests(unittest.TestCase):
    def bound_receipt(self):
        head = MODULE.subprocess.run(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, check=True,
            capture_output=True, text=True,
        ).stdout.strip()
        trace = ROOT / "test/fixtures/perfetto-gpu/first-frame-pipeline-upload-stall.pftrace"
        binding = MODULE.source_binding(ROOT, head, trace)
        return {
            "source_revision": head,
            "integration_head": binding["integration_head"],
            "source_blobs": binding["source_blobs"],
            "artifacts": {"trace": {
                "role": "repository/test/fixtures/perfetto-gpu/"
                        "first-frame-pipeline-upload-stall.pftrace"
            }},
        }

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
                "observed_spans": [
                    {
                        "name": "gpu_pipeline_prepare", "duration_ns": 1_800_000,
                        "gpu_evidence_id": "44444444444444444444444444444444",
                        "frame_index": 0, "sequence": 1, "health_state": "healthy",
                    },
                    {
                        "name": "gpu_resource_upload", "duration_ns": 900_000,
                        "gpu_evidence_id": "44444444444444444444444444444444",
                        "frame_index": 0, "sequence": 2, "health_state": "healthy",
                    },
                ],
            },
        }

    @staticmethod
    def semantic_result():
        return {
            "contributors": [
                {
                    "stage": "pipeline-prepare", "duration_ns": 1_800_000,
                    "evidence_id": "44444444444444444444444444444444",
                    "frame_index": 0, "sequence": 1, "health_state": "healthy",
                },
                {
                    "stage": "resource-upload", "duration_ns": 900_000,
                    "evidence_id": "44444444444444444444444444444444",
                    "frame_index": 0, "sequence": 2, "health_state": "healthy",
                },
            ]
        }

    def test_preserves_exact_passing_human_review_for_same_startup_trace(self):
        receipt = self.human_review_receipt()
        result = MODULE.preserve_human_perfetto_ui_correlation(
            receipt, question="gpu-startup", trace_sha256="1" * 64,
            semantic_result=self.semantic_result(),
        )
        self.assertEqual(result, receipt["human_perfetto_ui_correlation"])

    def test_mcp_response_timeout_terminates_the_child(self):
        session = MODULE.McpSession(Path("/bin/cat"), {})
        with mock.patch.object(MODULE.select, "select", return_value=([], [], [])):
            with self.assertRaisesRegex(RuntimeError, "response exceeded"):
                session._request("initialize")
        session.close()
        self.assertIsNotNone(session.process.returncode)

    def test_terminal_status_requires_the_exact_measurement_protocol(self):
        human = {"reviewer": "independent"}
        self.assertEqual(
            MODULE.terminal_acceptance_status(
                human, warmups=5, trials=30, fresh_start_trials=20
            ),
            "pass",
        )
        self.assertEqual(
            MODULE.terminal_acceptance_status(
                human, warmups=0, trials=2, fresh_start_trials=1
            ),
            "nonterminal-reduced-measurement-protocol",
        )
        self.assertEqual(
            MODULE.terminal_acceptance_status(
                None, warmups=5, trials=30, fresh_start_trials=20
            ),
            "nonterminal-missing-human-perfetto-correlation",
        )

    def test_human_review_rejects_different_trace_digest(self):
        with self.assertRaisesRegex(ValueError, "not bound to the measured trace"):
            MODULE.preserve_human_perfetto_ui_correlation(
                self.human_review_receipt(),
                question="gpu-startup", trace_sha256="2" * 64,
                semantic_result=self.semantic_result(),
            )

    def test_human_review_rejects_mismatched_prior_trace_artifact(self):
        receipt = self.human_review_receipt()
        receipt["artifacts"]["trace"]["sha256"] = "2" * 64
        with self.assertRaisesRegex(ValueError, "not bound to its receipt trace"):
            MODULE.preserve_human_perfetto_ui_correlation(
                receipt, question="gpu-startup", trace_sha256="1" * 64,
                semantic_result=self.semantic_result(),
            )

    def test_human_review_rejects_non_startup_question(self):
        with self.assertRaisesRegex(ValueError, "only to gpu-startup"):
            MODULE.preserve_human_perfetto_ui_correlation(
                self.human_review_receipt(),
                question="gpu-health", trace_sha256="1" * 64,
                semantic_result=self.semantic_result(),
            )

    def test_human_review_rejects_missing_acceptance_or_root_object(self):
        missing_acceptance = self.human_review_receipt()
        del missing_acceptance["acceptance"]
        with self.assertRaisesRegex(ValueError, "lacks passing"):
            MODULE.preserve_human_perfetto_ui_correlation(
                missing_acceptance, question="gpu-startup", trace_sha256="1" * 64,
                semantic_result=self.semantic_result(),
            )
        missing_root = self.human_review_receipt()
        del missing_root["human_perfetto_ui_correlation"]
        with self.assertRaisesRegex(ValueError, "lacks the root correlation object"):
            MODULE.preserve_human_perfetto_ui_correlation(
                missing_root, question="gpu-startup", trace_sha256="1" * 64,
                semantic_result=self.semantic_result(),
            )

    def test_human_review_rejects_non_passing_prior_acceptance(self):
        receipt = self.human_review_receipt()
        receipt["acceptance"]["human_perfetto_ui_correlation"] = "unverified"
        with self.assertRaisesRegex(ValueError, "lacks passing"):
            MODULE.preserve_human_perfetto_ui_correlation(
                receipt, question="gpu-startup", trace_sha256="1" * 64,
                semantic_result=self.semantic_result(),
            )

    def test_human_review_rejects_stripped_visual_review_fields(self):
        for field in ("reviewer", "reviewed_utc", "ui_revision", "delivery"):
            with self.subTest(field=field):
                receipt = self.human_review_receipt()
                del receipt["human_perfetto_ui_correlation"][field]
                with self.assertRaisesRegex(ValueError, f"lacks nonempty {field}"):
                    MODULE.preserve_human_perfetto_ui_correlation(
                        receipt, question="gpu-startup", trace_sha256="1" * 64,
                        semantic_result=self.semantic_result(),
                    )
        receipt = self.human_review_receipt()
        receipt["human_perfetto_ui_correlation"]["observed_spans"] = []
        with self.assertRaisesRegex(ValueError, "lacks observed span details"):
            MODULE.preserve_human_perfetto_ui_correlation(
                receipt, question="gpu-startup", trace_sha256="1" * 64,
                semantic_result=self.semantic_result(),
            )

    def test_human_review_rejects_prior_non_startup_receipt(self):
        receipt = self.human_review_receipt()
        receipt["protocol"]["question"] = "gpu-health"
        with self.assertRaisesRegex(ValueError, "must be for gpu-startup"):
            MODULE.preserve_human_perfetto_ui_correlation(
                receipt, question="gpu-startup", trace_sha256="1" * 64,
                semantic_result=self.semantic_result(),
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

    def test_mixed_install_prefix_cannot_borrow_current_build_stamp(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            build = root / "build"
            prefix = root / "prefix"
            pairs = {
                "pulp": (build / "pulp", prefix / "bin/pulp", b"rust-front"),
                "pulp-cpp": (
                    build / "tools/cli/pulp-cpp", prefix / "bin/pulp-cpp",
                    b"cpp-delegate",
                ),
                "pulp-mcp": (
                    build / "tools/mcp/pulp-mcp", prefix / "bin/pulp-mcp",
                    b"mcp-server",
                ),
            }
            for built, installed, payload in pairs.values():
                built.parent.mkdir(parents=True, exist_ok=True)
                installed.parent.mkdir(parents=True, exist_ok=True)
                built.write_bytes(payload)
                installed.write_bytes(payload)
                os.chmod(built, 0o755)
                os.chmod(installed, 0o755)
            identity = MODULE._build_install_binary_identity(build, prefix)
            self.assertEqual(
                identity["pulp-mcp"]["sha256"],
                identity["pulp-mcp"]["build_output_sha256"],
            )
            stale_mcp = pairs["pulp-mcp"][1]
            stale_mcp.write_bytes(b"stale-mixed-prefix-mcp")
            os.chmod(stale_mcp, 0o755)
            with self.assertRaisesRegex(ValueError, "differ from the exact build output"):
                MODULE._build_install_binary_identity(build, prefix)

    def test_output_must_be_new_and_outside_every_protected_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            external = Path(temporary)
            allowed = external / "receipt.json"
            MODULE.validate_output_path(allowed, (ROOT,))

            with self.assertRaisesRegex(ValueError, "outside every protected tree"):
                MODULE.validate_output_path(ROOT / "receipt.json", (ROOT,))

            existing = external / "existing.json"
            existing.write_text("do not overwrite", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "must be a new path"):
                MODULE.validate_output_path(existing, (ROOT,))

            symlink = external / "receipt-link.json"
            symlink.symlink_to(existing)
            with self.assertRaisesRegex(ValueError, "must be a new path"):
                MODULE.validate_output_path(symlink, (ROOT,))

            with self.assertRaises(FileExistsError):
                MODULE.atomic_write_json(existing, {"must_not": "replace"})
            self.assertEqual(existing.read_text(encoding="utf-8"), "do not overwrite")

            published = external / "published.json"
            MODULE.atomic_write_json(published, {"status": "pass"})
            self.assertEqual(json.loads(published.read_text()), {"status": "pass"})

    def test_output_publication_rejects_parent_directory_swap(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            parent = root / "parent"
            parent.mkdir()
            moved = root / "moved-parent"
            output = parent / "receipt.json"
            real_link = MODULE.os.link

            def swap_parent_before_link(*args, **kwargs):
                parent.rename(moved)
                parent.mkdir()
                return real_link(*args, **kwargs)

            with mock.patch.object(MODULE.os, "link", side_effect=swap_parent_before_link):
                with self.assertRaisesRegex(ValueError, "output-parent"):
                    MODULE.atomic_write_json(output, {"status": "pass"})
            self.assertFalse(output.exists())
            self.assertFalse((moved / "receipt.json").exists())

    def test_output_publication_rejects_staged_inode_swap(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "receipt.json"
            real_link = MODULE.os.link

            def swap_staged_file_before_link(source, destination, **kwargs):
                staged = root / source
                staged.rename(root / "original.tmp")
                staged.write_text('{"forged":true}\n', encoding="utf-8")
                return real_link(source, destination, **kwargs)

            with mock.patch.object(
                MODULE.os, "link", side_effect=swap_staged_file_before_link
            ):
                with self.assertRaisesRegex(ValueError, "identity changed"):
                    MODULE.atomic_write_json(output, {"status": "pass"})
            self.assertFalse(output.exists())

    def test_output_publication_rejects_in_place_receipt_mutation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            output = root / "receipt.json"
            real_link = MODULE.os.link

            def mutate_after_link(source, destination, **kwargs):
                result = real_link(source, destination, **kwargs)
                output.write_text('{"forged":true}\n', encoding="utf-8")
                return result

            with mock.patch.object(MODULE.os, "link", side_effect=mutate_after_link):
                with self.assertRaisesRegex(ValueError, "bytes differ"):
                    MODULE.atomic_write_json(output, {"status": "pass"})
            self.assertFalse(output.exists())

    def test_claimed_install_prefix_rejects_path_substitution(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            prefix = root / "prefix"
            prefix.mkdir()
            descriptor = os.open(prefix, MODULE._directory_open_flags())
            claim = MODULE._directory_identity(os.fstat(descriptor))
            moved = root / "moved-prefix"
            prefix.rename(moved)
            prefix.mkdir()
            try:
                with self.assertRaisesRegex(ValueError, "no longer names"):
                    MODULE._assert_directory_path_identity(
                        prefix, descriptor, claim, "install-prefix"
                    )
            finally:
                os.close(descriptor)

    def test_retained_executable_claim_rejects_child_inode_swap(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            prefix = root / "prefix"
            binary = prefix / "bin/pulp"
            binary.parent.mkdir(parents=True)
            binary.write_bytes(b"claimed executable")
            descriptor = os.open(prefix, MODULE._directory_open_flags())
            claim = MODULE.RetainedDirectoryClaim(
                prefix,
                descriptor,
                MODULE._directory_identity(os.fstat(descriptor)),
                "install-prefix",
            )
            claim.bind_file(binary, "installed Rust CLI", MODULE.sha256(binary))
            claim.seal()
            moved = binary.with_name("pulp-original")
            binary.rename(moved)
            binary.write_bytes(b"substituted executable")
            binary.unlink()
            moved.rename(binary)
            try:
                with self.assertRaisesRegex(ValueError, "mutation event"):
                    claim.assert_current()
            finally:
                claim.close()

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

    def test_checked_in_receipt_is_stale_until_exact_source_binding_is_regenerated(self):
        receipt = json.loads(
            (ROOT / "docs" / "validation" / "gpu-trace-overhead" /
             "m3-a2t-offline-analysis-20260828.json").read_text(encoding="utf-8")
        )
        errors = MODULE.source_binding_errors(receipt, ROOT)
        self.assertIn("integration_head must be an exact Git SHA", errors)
        self.assertIn(
            (
                "source_blobs does not bind the exact A2T "
                "behavior/build/fixture/producer-authority set"
            ),
            errors,
        )

    def test_exact_current_source_binding_passes(self):
        self.assertEqual(MODULE.source_binding_errors(self.bound_receipt(), ROOT), [])

    def test_planted_stale_head_and_declared_blob_fail_closed(self):
        stale_head = self.bound_receipt()
        stale_head["integration_head"] = "0" * 40
        errors = MODULE.source_binding_errors(stale_head, ROOT)
        self.assertTrue(any("source blob mismatch" in error for error in errors))

        stale_blob = self.bound_receipt()
        target = "experimental/pulp-rs/src/cmd/trace_gpu_analysis.rs"
        stale_blob["source_blobs"][target] = "0" * 40
        errors = MODULE.source_binding_errors(stale_blob, ROOT)
        self.assertIn(f"source blob mismatch for {target}", errors)
        self.assertIn(f"current HEAD source blob drift for {target}", errors)

    def test_planted_current_head_and_checkout_drift_fail_closed(self):
        receipt = self.bound_receipt()
        target = ".agents/skills/trace-sql/pulp_gpu_probe_correlation.sql"
        original_git_blobs = MODULE.git_blobs

        def drifted_head(repository, revision, paths):
            blobs = original_git_blobs(repository, revision, paths)
            if revision == "HEAD":
                blobs[target] = "f" * 40
            return blobs

        with mock.patch.object(MODULE, "git_blobs", side_effect=drifted_head):
            errors = MODULE.source_binding_errors(receipt, ROOT)
        self.assertIn(f"current HEAD source blob drift for {target}", errors)

        checkout = MODULE.checkout_blobs(ROOT, set(receipt["source_blobs"]))
        checkout[target] = "e" * 40
        with mock.patch.object(MODULE, "checkout_blobs", return_value=checkout):
            errors = MODULE.source_binding_errors(receipt, ROOT)
        self.assertIn(f"current checkout source blob drift for {target}", errors)

    def test_checked_in_receipt_preserves_a3_human_review_binding(self):
        receipt = json.loads(
            (ROOT / "docs" / "validation" / "gpu-trace-overhead" /
             "m3-a2t-offline-analysis-20260828.json").read_text(encoding="utf-8")
        )
        trace_digest = receipt["artifacts"]["trace"]["sha256"]
        correlation = MODULE.preserve_human_perfetto_ui_correlation(
            receipt, question=receipt["protocol"]["question"],
            trace_sha256=trace_digest,
            semantic_result=receipt["semantic_result"],
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

    def test_mixed_commit_inventory_is_limited_to_a2t_manifest_paths(self):
        sql = ".agents/skills/trace-sql/pulp_gpu_startup_breakdown.sql"
        changed = [sql, "tools/cli/gpu_probe/src/dpr_measurement_session.cpp"]
        self.assertEqual(MODULE.path_limited_changed_paths(changed, {sql}), [sql])

    def test_scope_inventory_rejects_moving_ref(self):
        with self.assertRaises(ValueError):
            MODULE.a2t_scope_inventory(Path("/repo"), "HEAD")

    def test_scope_manifest_matches_authoritative_current_path_contract(self):
        head = MODULE._git_text(ROOT, "rev-parse", "HEAD")
        manifest = MODULE._load_a2t_scope_manifest(ROOT, head)
        paths, accepted, external = MODULE.authoritative_a2t_scope_paths(ROOT, head)
        self.assertEqual(set(manifest["scope_paths"]), paths)
        self.assertEqual(
            len(accepted),
            manifest["accepted_plan_implementation"]["path_count"],
        )
        self.assertEqual(
            [row["path"] for row in external],
            ["inspect/src/control_gpu_health_provider.cpp"],
        )
        self.assertEqual(
            external[0]["introducing_revision"],
            "8175bd483f5e4ca66989c9ad91a4d9ed5a864bb0",
        )
        self.assertTrue(
            external[0]["scope_authority"][
                "producer_introduction_touched_authority_path"
            ]
        )
        self.assertEqual(
            external[0]["owner_evidence_status"],
            "external-not-evaluated-by-a2t",
        )

    def test_scope_manifest_cannot_omit_independently_discovered_semantic_path(self):
        head = MODULE._git_text(ROOT, "rev-parse", "HEAD")
        path = ROOT / MODULE.A2T_SCOPE_MANIFEST_PATH
        manifest = json.loads(path.read_text())
        manifest["scope_paths"].remove(".claude/commands/trace.md")
        with mock.patch.object(MODULE.json, "loads", return_value=manifest):
            with self.assertRaisesRegex(ValueError, "independently discovered"):
                MODULE._load_a2t_scope_manifest(ROOT, head)

    def test_semantic_discovery_uses_exact_identifiers_not_incidental_symptoms(self):
        self.assertTrue(
            MODULE.has_a2t_semantic_identifier(
                'schema = "pulp.trace-gpu-analysis.v1"'
            )
        )
        self.assertTrue(
            MODULE.has_a2t_semantic_identifier(
                'TRACE_EVENT("gpu", "gpu_submit", "debug.gpu_evidence_id", id)'
            )
        )
        self.assertIn("core/render", MODULE.A2T_SEMANTIC_DISCOVERY_PATHS)
        self.assertFalse(
            MODULE.has_a2t_semantic_identifier(
                "GPU startup tracing can help explain a slow first frame."
            )
        )

    def test_product_producer_discovery_uses_actual_source_macro_form(self):
        source = (
            'PULP_TRACE_SCOPE_NAMED_ARGS("gpu", "gpu_health_transition", '
            '"gpu_evidence_id", value);'
        )
        self.assertTrue(MODULE.is_product_producer_source(source))
        self.assertTrue(
            MODULE.is_product_producer_source(
                'PULP_TRACE_SCOPE_NAMED("gpu", "pipeline_compile");'
            )
        )
        self.assertTrue(
            MODULE.is_product_producer_source(
                'if (enabled) PULP_TRACE_SCOPE_NAMED("gpu", "pipeline_compile");'
            )
        )
        self.assertTrue(
            MODULE.is_product_producer_source(
                'do { PULP_TRACE_COUNTER("gpu", "queued", count); } while (false);'
            )
        )
        self.assertFalse(
            MODULE.is_product_producer_source(
                'config.gpu_evidence_id = "gpu_evidence_id";'
            )
        )
        self.assertFalse(
            MODULE.is_product_producer_source(
                'PULP_TRACE_SCOPE_NAMED("state", "pointer_dispatch");'
            )
        )
        self.assertFalse(
            MODULE.is_product_producer_source(
                '// PULP_TRACE_SCOPE_NAMED("gpu", "not_a_call");\n'
                'const char* text = "PULP_TRACE_SCOPE_NAMED(\\\"gpu\\\", '
                '\\\"also_not_a_call\\\");";'
            )
        )
        self.assertEqual(
            MODULE.product_producer_signatures(
                'PULP_TRACE_SCOPE_NAMED("gpu", /* incidental */ "gpu_submit");'
            ),
            MODULE.product_producer_signatures(
                'PULP_TRACE_SCOPE_NAMED("gpu", "gpu_submit");'
            ),
        )
        oversized = (
            'PULP_TRACE_SCOPE_NAMED("gpu", "'
            + ("x" * MODULE.MAX_PRODUCT_TRACE_CALL_BYTES)
            + '");'
        )
        with self.assertRaisesRegex(ValueError, "exceeds the bounded scan"):
            MODULE.product_producer_signatures(oversized)

    def test_product_producer_delta_catches_gpu_call_without_evidence_literal(self):
        before = 'PULP_TRACE_SCOPE_NAMED("gpu", "gpu_submit");'
        after = before + '\nPULP_TRACE_SCOPE_NAMED("gpu", "pipeline_compile");'
        before_signatures = MODULE.product_producer_signatures(before)
        after_signatures = MODULE.product_producer_signatures(after)
        self.assertTrue(
            MODULE.has_added_product_producer(before_signatures, after_signatures)
        )
        replacement = MODULE.product_producer_signatures(
            'PULP_TRACE_SCOPE_NAMED("gpu", "pipeline_compile");'
        )
        self.assertEqual(len(before_signatures), len(replacement))
        self.assertTrue(
            MODULE.has_added_product_producer(before_signatures, replacement)
        )

    def test_product_producer_must_have_independent_package_authority(self):
        head = MODULE._git_text(ROOT, "rev-parse", "HEAD")
        path = "inspect/src/control_gpu_health_provider.cpp"
        with mock.patch.object(MODULE, "_a3_authority_paths", return_value=(set(), "a" * 40)):
            with self.assertRaisesRegex(ValueError, "unclassified product producer"):
                MODULE.classify_non_a2t_product_producers(ROOT, head, {path})

    def test_product_producer_introduction_must_touch_package_authority(self):
        head = MODULE._git_text(ROOT, "rev-parse", "HEAD")
        path = "inspect/src/control_gpu_health_provider.cpp"
        with (
            mock.patch.object(
                MODULE, "_a3_authority_paths", return_value=({path}, "a" * 40)
            ),
            mock.patch.object(
                MODULE, "_producer_introduction",
                return_value=("b" * 40, "feat(gpu): unrelated producer"),
            ),
            mock.patch.object(MODULE, "_revision_changed_paths", return_value={path}),
        ):
            with self.assertRaisesRegex(ValueError, "unclassified product producer"):
                MODULE.classify_non_a2t_product_producers(ROOT, head, {path})


if __name__ == "__main__":
    unittest.main()
