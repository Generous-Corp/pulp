#!/usr/bin/env python3
"""Tests for desktop action label helper bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("desktop_action_label_bindings.py")


class DesktopActionLabelBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_label_exports_match_wrappers(self) -> None:
        expected = ("default_desktop_label",)

        self.assertEqual(self.mod.DESKTOP_ACTION_LABEL_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_label_wrapper_delegates_arguments(self) -> None:
        captured = {}

        def default_label(*args, **kwargs):
            captured["label"] = (args, kwargs)
            return "Demo"

        bindings = {
            "_desktop_actions": types.SimpleNamespace(default_desktop_label=default_label),
        }

        self.assertEqual(self.mod.default_desktop_label(bindings, "./Demo", bundle_id="com.example.Demo"), "Demo")
        self.assertEqual(captured["label"], (("./Demo",), {"bundle_id": "com.example.Demo"}))

if __name__ == "__main__":
    unittest.main()
