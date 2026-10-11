#!/usr/bin/env python3
import unittest
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


if __name__ == "__main__":
    unittest.main()
