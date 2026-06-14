#!/usr/bin/env python3
"""Tests for desktop source manifest dependency bindings."""

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("desktop_source_request_manifest_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopSourceRequestManifestBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_manifest_exports_match_wrappers(self):
        expected = ("attach_desktop_source_to_manifest",)

        self.assertEqual(self.mod.DESKTOP_SOURCE_REQUEST_MANIFEST_EXPORTS, expected)
        self.assertTrue(callable(self.mod.attach_desktop_source_to_manifest))

    def test_manifest_helper_delegates(self):
        captured = {}

        def attach_manifest(manifest, source_context):
            captured["attach"] = (manifest, source_context)

        bindings = {
            "_source_prep": types.SimpleNamespace(attach_desktop_source_to_manifest=attach_manifest),
        }
        manifest = {}
        source_context = {"mode": "live"}

        self.mod.attach_desktop_source_to_manifest(bindings, manifest, source_context)

        self.assertEqual(captured["attach"], (manifest, source_context))

if __name__ == "__main__":
    unittest.main()
