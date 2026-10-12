#!/usr/bin/env python3
import unittest
import json
import tempfile
from pathlib import Path
from argparse import Namespace
from vellum_closure import (
    external_input_violations,
    edge_key,
    load_allowlist,
    policy_include_forbidden,
    policy_target_forbidden,
    receipt_path_violations,
    symbol_closure,
)


class ClosurePolicyTests(unittest.TestCase):
    def test_policy_matches_boundaries(self):
        self.assertTrue(policy_target_forbidden("pulp::audio"))
        self.assertTrue(policy_target_forbidden("pulp::format-view"))
        self.assertTrue(policy_target_forbidden("pulp::gpu_audio"))
        self.assertFalse(policy_target_forbidden("pulp::canvas"))
        self.assertFalse(policy_target_forbidden("pulp::audiofx"))

    def test_include_policy_negative_control(self):
        self.assertTrue(policy_include_forbidden("pulp/audio/buffer.hpp"))
        self.assertTrue(policy_include_forbidden("pulp/content/registry.hpp"))
        self.assertFalse(policy_include_forbidden("pulp/canvas/canvas.hpp"))
        self.assertFalse(policy_include_forbidden("pulp/audiofx/effect.hpp"))

    def test_symbol_policy_negative_control(self):
        self.assertEqual(symbol_closure(["_ZN5pulp14ContentRegistryE", "_ZN5pulp6CanvasE"]), ["_ZN5pulp14ContentRegistryE"])
        self.assertEqual(symbol_closure(["_ZN5pulp14ContentRegistryE"], ["_ZN5pulp14ContentRegistryE"]), [])

    def test_unknown_build_tree_input_is_rejected(self):
        manifest = {
            "source_root": ".",
            "external_inputs": [{
                "path": "@build/_deps/rogue-src/include/should-export.hpp",
                "source": "build-tree",
                "pin": "unresolved",
            }],
        }
        self.assertTrue(external_input_violations(manifest))

    def test_receipt_paths_have_no_host_absolute_values(self):
        self.assertEqual(receipt_path_violations({
            "files": [{"pulp_path": "core/canvas/canvas.cpp", "mirror_path": "mirror/core/canvas/canvas.cpp"}],
            "external_inputs": [{"path": "@build/_deps/choc-src/include/choc.hpp"}],
            "generated_inputs": [{"path": "@build/core/runtime/generated/build_info.hpp", "producer_paths": ["core/runtime/CMakeLists.txt"]}],
        }), [])
        self.assertTrue(receipt_path_violations({"files": [{"pulp_path": "/Users/agent/build/rogue.hpp"}]}))

    def test_edge_allowlist_is_exact_and_canonicalizes_p4_target(self):
        self.assertEqual(edge_key("pulp-design-import", "pulp-audio"), "pulp-design-import->pulp-view-audio")
        entries = load_allowlist(__import__("pathlib").Path(__file__).with_name("vellum_closure_allowlist.json"))
        self.assertTrue(any(e["kind"] == "edge" and e["match"] == "pulp-design-import->pulp-view-audio" for e in entries))
        self.assertFalse(any(e["kind"] == "target" for e in entries))

    def test_include_allowlist_is_edge_specific(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            source = root / "core/platform/src/android/jni_motion.cpp"
            source.parent.mkdir(parents=True)
            source.write_text("#include <pulp/audio/secret.hpp>\n", encoding="utf-8")
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps({
                "source_root": str(root),
                "files": ["core/platform/src/android/jni_motion.cpp"],
                "targets": [],
                "undefined_symbols": [],
            }), encoding="utf-8")
            args = Namespace(
                manifest=[str(manifest)], allow=[], allow_include_path=[],
                allow_include_edge=[("core/platform/src/android/jni_motion.cpp", "pulp/view/motion.hpp")],
                allow_include_prefix=[], allow_root_prefix=[], report_only=False,
                allowlist=None, generate=False, unit_manifest=None, target=None,
                source_root=None, build_dir=None,
            )
            self.assertEqual(__import__("vellum_closure").cmd_gate(args), 1)


if __name__ == "__main__":
    unittest.main()
