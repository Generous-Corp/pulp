#!/usr/bin/env python3
import unittest
from vellum_closure import (
    external_input_violations,
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


if __name__ == "__main__":
    unittest.main()
