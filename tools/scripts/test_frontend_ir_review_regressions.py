#!/usr/bin/env python3
"""Regression tests for the fail-open / correctness fixes from the PR #3128 deep review.

Each test pins one previously-confirmed bug so the fix cannot silently regress.
See planning/2026-05-29-frontend-ir-review-findings.md for the full ledger.
"""

from __future__ import annotations

import importlib.util
import pathlib
import sys
import tempfile
import unittest


SCRIPT_DIR = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(SCRIPT_DIR))


def _load(name: str):
    spec = importlib.util.spec_from_file_location(name, SCRIPT_DIR / f"{name}.py")
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


proofs = _load("frontend_ir_proofs")
session = _load("frontend_ir_session")
tokens = _load("frontend_ir_tokens")
inspector = _load("frontend_ir_inspector")
nvg = _load("frontend_ir_native_validation_gate")
gate = _load("frontend_ir_gate")
primitives = _load("frontend_ir_primitives")
primitive_gate = _load("frontend_ir_primitive_gate")
codegen = _load("frontend_ir_codegen_artifacts")
session_manifest = _load("frontend_ir_session_manifest")


def _status(checks: list[dict], check_id: str) -> str:
    for c in checks:
        if c.get("id") == check_id:
            return c.get("status")
    raise AssertionError(f"check {check_id!r} not found in {[c.get('id') for c in checks]}")


class ProofsFixtureMatch(unittest.TestCase):
    def test_empty_fixture_ids_do_not_match(self):
        # #9: two fixture-less artifacts both reporting "" must not match.
        self.assertFalse(proofs.row_matches_report({"fixture_id": ""}, {"fixture_id": ""}))

    def test_matching_non_empty_fixture_id_matches(self):
        self.assertTrue(proofs.row_matches_report({"fixture_id": "fx"}, {"fixture_id": "fx"}))

    def test_empty_report_still_matches_on_source_path(self):
        self.assertTrue(proofs.row_matches_report(
            {"path": "a/b.tsx"}, {"fixture_id": "", "source": {"path": "a/b.tsx"}}))


class SessionReloadPath(unittest.TestCase):
    def test_validation_only_is_validation_refresh(self):
        # #11: validation-only delta must not be swallowed into token_tweak_reload.
        self.assertEqual(session.recommended_reload(["validation"], True), "validation_refresh")

    def test_tokens_still_token_tweak_reload(self):
        self.assertEqual(session.recommended_reload(["tokens"], True), "token_tweak_reload")

    def test_route_change_forces_full_reimport(self):
        self.assertEqual(session.recommended_reload(["routes"], True), "full_reimport")


class TokenColorTyping(unittest.TestCase):
    def test_functional_colors_typed_as_color(self):
        # #13: rgb()/rgba()/hsl()/hsla() are colors, not bare strings.
        for v in ("rgb(1,2,3)", "rgba(1,2,3,0.5)", "hsl(200,50%,50%)", "HSLA(0,0%,0%,1)"):
            self.assertEqual(tokens.infer_resolved_type(v), "color", v)

    def test_hex_still_color_and_plain_string_unchanged(self):
        self.assertEqual(tokens.infer_resolved_type("#fff"), "color")
        self.assertEqual(tokens.infer_resolved_type("Inter"), "string")


class InspectorClassificationPreserved(unittest.TestCase):
    def test_route_invalidating_tweak_is_not_preserving(self):
        # #12: derive from invalidates, do not trust producer flag.
        tweak = {"invalidates": ["route"], "classification_preserved": True}
        self.assertFalse(inspector.tweak_preserves_classification(tweak))

    def test_style_only_tweak_preserves(self):
        self.assertTrue(inspector.tweak_preserves_classification(
            {"invalidates": ["style"], "classification_preserved": True}))

    def test_explicit_false_respected(self):
        self.assertFalse(inspector.tweak_preserves_classification(
            {"invalidates": [], "classification_preserved": False}))


class BehaviorVisualThreshold(unittest.TestCase):
    def _report(self, **over):
        base = {
            "schema": nvg.BEHAVIOR_VISUAL_SCHEMA,
            "compiled_with_cpp_only_flag": True,
            "target_links_script_runtime": False,
            "within_threshold": True,
            "full_within_threshold": True,
            "threshold": 0.9,
            "full_similarity": 0.99,
            "routed_region_similarity": 0.99,
            "behavior": {"passed": True, "interaction_count": 3, "parameter_event_count": 2},
            "bound_counts": {"parameters": 5},
        }
        base.update(over)
        return base

    def test_missing_threshold_fails(self):
        # #4: threshold<=0 makes the similarity comparison toothless; must fail.
        checks = nvg.behavior_visual_checks(self._report(threshold=0.0, full_similarity=0.0,
                                                         routed_region_similarity=0.0))
        self.assertEqual(_status(checks, "behavior_visual_parity"), nvg.FAIL_STATUS)

    def test_real_threshold_passes(self):
        checks = nvg.behavior_visual_checks(self._report())
        self.assertEqual(_status(checks, "behavior_visual_parity"), nvg.PASS_STATUS)


class ChildGateEmptyChecks(unittest.TestCase):
    def test_empty_checks_child_fails(self):
        # #5: a "ready" child with no checks verified nothing.
        report = {"schema": "pulp-frontend-ir-gate-v0", "verdict": nvg.READY_VERDICT,
                  "summary": {"failures": 0, "warnings": 0}, "checks": []}
        checks = nvg.child_gate_check(report, expected_schema="pulp-frontend-ir-gate-v0", check_id="x_gate", label="X gate")
        self.assertEqual(_status(checks, "x_gate"), nvg.FAIL_STATUS)

    def test_child_with_checks_passes(self):
        report = {"schema": "pulp-frontend-ir-gate-v0", "verdict": nvg.READY_VERDICT,
                  "summary": {"failures": 0, "warnings": 0},
                  "checks": [{"id": "c1", "status": "pass"}]}
        checks = nvg.child_gate_check(report, expected_schema="pulp-frontend-ir-gate-v0", check_id="x_gate", label="X gate")
        self.assertEqual(_status(checks, "x_gate"), nvg.PASS_STATUS)


class GateBinaryProof(unittest.TestCase):
    def test_empty_dict_is_not_proof(self):
        # #10: a bare {} artifact is not proof — must not satisfy no-JS.
        self.assertFalse(gate.refs_real_artifact({}))
        self.assertFalse(gate.refs_real_artifact({"path": ""}))
        self.assertFalse(gate.refs_real_artifact(None))

    def test_artifact_with_path_is_proof(self):
        self.assertTrue(gate.refs_real_artifact({"path": "reports/proof.json"}))


class PrimitiveRouteMembership(unittest.TestCase):
    def _report(self):
        # Two nodes, but only one routed -> count-equality could otherwise hide
        # the gap if a duplicate/orphan route balanced the count.
        return {
            "schema": "pulp-frontend-ir-v0",
            "fixture_id": "fx",
            "nodes": [
                {"id": "n1", "semantic_role": "knob", "source_span": {"path": "x:1"},
                 "style": {}, "state": {}},
                {"id": "n2", "semantic_role": "knob", "source_span": {"path": "x:2"},
                 "style": {}, "state": {}},
            ],
            # one real route + one orphan route -> len(routes)==len(nodes) but n2 uncovered
            "routes": [
                {"node_id": "n1", "chosen_route": "native_cpp", "requires_js_engine": False},
                {"node_id": "orphan", "chosen_route": "native_cpp", "requires_js_engine": False},
            ],
            "validation": {},
        }

    def test_nodes_without_route_counted(self):
        # #8
        report = primitives.build_primitive_report(self._report())
        self.assertEqual(report["summary"]["nodes_without_route"], 1)

    def test_route_coverage_fails_on_missing_membership(self):
        # #8: gate must fail even though len(routes)==len(nodes).
        coverage = primitives.build_primitive_report(self._report())
        checks = primitive_gate.coverage_checks(coverage)
        self.assertEqual(_status(checks, "route_coverage"), primitive_gate.FAIL_STATUS)

    def test_unrouted_node_counts_as_js_requiring(self):
        # #7: a node with no proven no-JS route must count as JS-requiring.
        report = primitives.build_primitive_report(self._report())
        self.assertGreaterEqual(report["summary"]["nodes_requiring_js"], 1)


class CodegenSplitPrefix(unittest.TestCase):
    def test_route_owned_prefix_entry_not_counted_as_split(self):
        # #6: "foo.label" that directly binds its OWN route must not account for
        # an unrelated unbound route "foo".
        frontend_ir = {
            "routes": [
                {"node_id": "foo", "chosen_route": codegen.NATIVE_CPP_ROUTE},
                {"node_id": "foo.label", "chosen_route": codegen.NATIVE_CPP_ROUTE},
            ],
        }
        manifest = {"schema": codegen.BINDING_SCHEMA, "entries": [{"id": "foo.label"}]}
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            for name in ("ir.json", "ui.cpp", "ui.hpp", "ui.bindings.json"):
                (root / name).write_text("{}")
            report = codegen.build_codegen_artifact_report(
                frontend_ir, manifest,
                frontend_ir_path=root / "ir.json",
                source_cpp=root / "ui.cpp",
                header=root / "ui.hpp",
                binding_manifest_path=root / "ui.bindings.json",
                repo_root=root,
            )
        self.assertIn("foo", report["missing_native_route_bindings"])
        self.assertEqual(report["split_binding_candidates"], [])


class SessionManifestEmptyFile(unittest.TestCase):
    def test_empty_file_byte_size_zero_overwrites_stale(self):
        # #14: an on-disk empty file (byte_size 0) must overwrite the IR value.
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            empty = root / "asset.bin"
            empty.write_bytes(b"")
            meta = session_manifest.local_metadata(empty, root)
            self.assertEqual(meta["byte_size"], 0)
            entry = {"byte_size": 9999}
            for key in ("sha256", "byte_size", "mime"):
                if meta.get(key) is not None:
                    entry[key] = meta[key]
            self.assertEqual(entry["byte_size"], 0)


if __name__ == "__main__":
    unittest.main()
