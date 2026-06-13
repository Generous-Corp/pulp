#!/usr/bin/env python3
"""Tests for desktop source cache/root dependency bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("desktop_source_request_path_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopSourceRequestPathBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_path_exports_match_wrappers(self):
        expected = (
            "desktop_source_cache_key",
            "desktop_source_root",
        )

        self.assertEqual(self.mod.DESKTOP_SOURCE_REQUEST_PATH_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_path_helpers_bind_dependencies(self):
        captured = {}

        def source_cache_key(source_request):
            captured["cache_key"] = source_request
            return "abc123"

        def source_root(*args, **kwargs):
            captured["source_root"] = (args, kwargs)
            return Path("/state/desktop-source/mac/abc123")

        bindings = {
            "_source_prep": types.SimpleNamespace(
                desktop_source_cache_key=source_cache_key,
                desktop_source_root=source_root,
            ),
            "state_dir": object(),
        }
        request = {"sha": "abc"}

        self.assertEqual(self.mod.desktop_source_cache_key(bindings, request), "abc123")
        self.assertIs(captured["cache_key"], request)
        self.assertEqual(self.mod.desktop_source_root(bindings, "mac", request), Path("/state/desktop-source/mac/abc123"))
        self.assertEqual(captured["source_root"][0], ("mac", request))
        self.assertIs(captured["source_root"][1]["state_dir_fn"], bindings["state_dir"])

    def test_path_installer_wires_selected_helper(self):
        bindings = {
            "_source_prep": types.SimpleNamespace(desktop_source_cache_key=lambda source_request: "abc123"),
        }

        self.mod.install_desktop_source_request_path_helpers(bindings, ("desktop_source_cache_key",))

        self.assertEqual(bindings["desktop_source_cache_key"]({"sha": "abc"}), "abc123")
        self.assertNotIn("desktop_source_root", bindings)

    def test_path_installer_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_desktop_source_request_path_helper = lambda _bindings: "future"

        self.mod.install_desktop_source_request_path_helpers(
            bindings,
            ("future_desktop_source_request_path_helper",),
        )

        self.assertEqual(bindings["future_desktop_source_request_path_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
