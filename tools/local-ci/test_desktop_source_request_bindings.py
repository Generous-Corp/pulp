#!/usr/bin/env python3
"""Tests for desktop source request dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_source_request_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopSourceRequestBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_source_request_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.DESKTOP_SOURCE_REQUEST_CORE_EXPORTS,
            *self.mod.DESKTOP_SOURCE_REQUEST_PATH_EXPORTS,
            *self.mod.DESKTOP_SOURCE_REQUEST_WINDOWS_EXPORTS,
            *self.mod.DESKTOP_SOURCE_REQUEST_MANIFEST_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_SOURCE_REQUEST_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_source_request_installer_routes_selected_groups(self):
        source_prep = types.SimpleNamespace(
            make_desktop_source_request=lambda args, **kwargs: {"mode": "live"},
            desktop_source_cache_key=lambda source_request: "abc123",
            split_windows_prepare_commands=lambda command: ["one", "two"],
            attach_desktop_source_to_manifest=lambda manifest, source_context: manifest.update({"source": source_context}),
        )
        bindings = {
            "_source_prep": source_prep,
            "normalize_desktop_source_mode": object(),
            "current_branch": object(),
            "current_sha": object(),
        }

        self.mod.install_desktop_source_request_helpers(
            bindings,
            (
                "make_desktop_source_request",
                "desktop_source_cache_key",
                "split_windows_prepare_commands",
                "attach_desktop_source_to_manifest",
            ),
        )

        manifest = {}
        self.assertEqual(bindings["make_desktop_source_request"](object()), {"mode": "live"})
        self.assertEqual(bindings["desktop_source_cache_key"]({"sha": "abc"}), "abc123")
        self.assertEqual(bindings["split_windows_prepare_commands"]("one;two"), ["one", "two"])
        bindings["attach_desktop_source_to_manifest"](manifest, {"mode": "live"})
        self.assertEqual(manifest, {"source": {"mode": "live"}})
        self.assertNotIn("desktop_source_root", bindings)
        self.assertNotIn("validate_windows_prepare_commands", bindings)

    def test_source_request_installer_preserves_unknown_fallback(self):
        bindings = {}

        with mock.patch.object(self.mod, "install_local_helpers") as install_local:
            self.mod.install_desktop_source_request_helpers(bindings, ("unknown_helper",))

        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))


if __name__ == "__main__":
    unittest.main()
