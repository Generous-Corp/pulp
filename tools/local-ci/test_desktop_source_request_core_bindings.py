#!/usr/bin/env python3
"""Tests for desktop source request creation dependency bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("desktop_source_request_core_bindings.py")


class DesktopSourceRequestCoreBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_core_exports_match_wrappers(self):
        expected = ("make_desktop_source_request",)

        self.assertEqual(self.mod.DESKTOP_SOURCE_REQUEST_CORE_EXPORTS, expected)
        self.assertTrue(callable(self.mod.make_desktop_source_request))

    def test_make_desktop_source_request_binds_dependencies(self):
        captured = {}

        def make_request(*args, **kwargs):
            captured["make_request"] = (args, kwargs)
            return {"mode": "live"}

        bindings = {
            "_source_prep": types.SimpleNamespace(make_desktop_source_request=make_request),
            "normalize_desktop_source_mode": object(),
            "current_branch": object(),
            "current_sha": object(),
        }
        args_obj = object()

        self.assertEqual(self.mod.make_desktop_source_request(bindings, args_obj), {"mode": "live"})
        self.assertEqual(captured["make_request"][0], (args_obj,))
        self.assertIs(captured["make_request"][1]["normalize_desktop_source_mode_fn"], bindings["normalize_desktop_source_mode"])
        self.assertIs(captured["make_request"][1]["current_branch_fn"], bindings["current_branch"])
        self.assertIs(captured["make_request"][1]["current_sha_fn"], bindings["current_sha"])

if __name__ == "__main__":
    unittest.main()
