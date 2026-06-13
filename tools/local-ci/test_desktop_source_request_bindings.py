#!/usr/bin/env python3
"""Tests for desktop source request dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_source_request_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopSourceRequestBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_source_request_cache_root_and_manifest_helpers_bind_dependencies(self):
        captured = {}

        def make_request(*args, **kwargs):
            captured["make_request"] = (args, kwargs)
            return {"mode": "live"}

        def source_cache_key(source_request):
            captured["cache_key"] = source_request
            return "abc123"

        def source_root(*args, **kwargs):
            captured["source_root"] = (args, kwargs)
            return Path("/state/desktop-source/mac/abc123")

        def split_windows(command):
            captured["split"] = command
            return ["one", "two"]

        def validate_windows(commands):
            captured["validate"] = commands

        def attach_manifest(manifest, source_context):
            captured["attach"] = (manifest, source_context)

        source_prep = types.SimpleNamespace(
            make_desktop_source_request=make_request,
            desktop_source_cache_key=source_cache_key,
            desktop_source_root=source_root,
            split_windows_prepare_commands=split_windows,
            validate_windows_prepare_commands=validate_windows,
            attach_desktop_source_to_manifest=attach_manifest,
        )
        bindings = {
            "_source_prep": source_prep,
            "normalize_desktop_source_mode": object(),
            "current_branch": object(),
            "current_sha": object(),
            "state_dir": object(),
        }
        args_obj = object()
        request = {"sha": "abc"}
        manifest = {}
        source_context = {"mode": "live"}

        self.assertEqual(self.mod.make_desktop_source_request(bindings, args_obj), {"mode": "live"})
        self.assertEqual(captured["make_request"][0], (args_obj,))
        self.assertIs(captured["make_request"][1]["normalize_desktop_source_mode_fn"], bindings["normalize_desktop_source_mode"])
        self.assertIs(captured["make_request"][1]["current_branch_fn"], bindings["current_branch"])
        self.assertIs(captured["make_request"][1]["current_sha_fn"], bindings["current_sha"])

        self.assertEqual(self.mod.desktop_source_cache_key(bindings, request), "abc123")
        self.assertIs(captured["cache_key"], request)

        self.assertEqual(self.mod.desktop_source_root(bindings, "mac", request), Path("/state/desktop-source/mac/abc123"))
        self.assertEqual(captured["source_root"][0], ("mac", request))
        self.assertIs(captured["source_root"][1]["state_dir_fn"], bindings["state_dir"])

        self.assertEqual(self.mod.split_windows_prepare_commands(bindings, "one;two"), ["one", "two"])
        self.assertEqual(captured["split"], "one;two")

        self.mod.validate_windows_prepare_commands(bindings, ["one"])
        self.assertEqual(captured["validate"], ["one"])

        self.mod.attach_desktop_source_to_manifest(bindings, manifest, source_context)
        self.assertEqual(captured["attach"], (manifest, source_context))

    def test_source_request_exports_are_composed_from_focused_groups(self):
        expected = (
            *self.mod.DESKTOP_SOURCE_REQUEST_CORE_EXPORTS,
            *self.mod.DESKTOP_SOURCE_REQUEST_PATH_EXPORTS,
            *self.mod.DESKTOP_SOURCE_REQUEST_WINDOWS_EXPORTS,
            *self.mod.DESKTOP_SOURCE_REQUEST_MANIFEST_EXPORTS,
        )

        self.assertEqual(self.mod.DESKTOP_SOURCE_REQUEST_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

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

    def test_install_desktop_source_request_helpers_keeps_unknown_local_fallback(self):
        bindings = {}
        self.mod.future_desktop_source_request_helper = lambda _bindings: "future"

        self.mod.install_desktop_source_request_helpers(bindings, ("future_desktop_source_request_helper",))

        self.assertEqual(bindings["future_desktop_source_request_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
