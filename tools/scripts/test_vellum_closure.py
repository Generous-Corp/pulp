#!/usr/bin/env python3
import unittest
import json
import tempfile
from pathlib import Path
from argparse import Namespace
from vellum_closure import policy_include_forbidden, policy_target_forbidden, symbol_closure


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
            )
            self.assertEqual(__import__("vellum_closure").cmd_gate(args), 1)


if __name__ == "__main__":
    unittest.main()
