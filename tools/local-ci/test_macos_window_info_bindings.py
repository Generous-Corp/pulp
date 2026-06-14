#!/usr/bin/env python3
"""Tests for macOS window info facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("macos_window_info_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class MacosWindowInfoBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, macos_desktop):
        return {
            "_macos_desktop": macos_desktop,
            "subprocess": types.SimpleNamespace(run=object()),
            "macos_window_probe_path": object(),
        }

    def test_info_exports_match_wrappers(self) -> None:
        expected = (
            "macos_window_info_for_pid",
            "macos_window_info_for_bundle_id",
            "macos_accessibility_trusted",
        )

        self.assertEqual(self.mod.MACOS_WINDOW_INFO_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_info_bindings_delegate_to_macos_desktop_module(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        run_fn = object()
        macos_desktop = types.SimpleNamespace(
            macos_window_info_for_pid=capture("pid_info", {"windows": [{"id": 7}]}),
            macos_window_info_for_bundle_id=capture("bundle_info", {"pid": 99, "windows": [{"id": 8}]}),
            macos_accessibility_trusted=capture("trusted", True),
        )
        bindings = self._bindings(macos_desktop)
        bindings["subprocess"] = types.SimpleNamespace(run=run_fn)

        self.assertEqual(self.mod.macos_window_info_for_pid(bindings, 123), {"windows": [{"id": 7}]})
        self.assertIs(captured["pid_info"][1]["probe_path_fn"], bindings["macos_window_probe_path"])
        self.assertIs(captured["pid_info"][1]["run_fn"], run_fn)
        self.assertEqual(self.mod.macos_window_info_for_bundle_id(bindings, "com.example.demo"), {"pid": 99, "windows": [{"id": 8}]})
        self.assertIs(captured["bundle_info"][1]["probe_path_fn"], bindings["macos_window_probe_path"])
        self.assertIs(captured["bundle_info"][1]["run_fn"], run_fn)
        self.assertTrue(self.mod.macos_accessibility_trusted(bindings))
        self.assertIs(captured["trusted"][1]["probe_path_fn"], bindings["macos_window_probe_path"])
        self.assertIs(captured["trusted"][1]["run_fn"], run_fn)

    def test_info_installer_wires_selected_helpers(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["trusted"] = (args, kwargs)
            return True

        macos_desktop = types.SimpleNamespace(macos_accessibility_trusted=runner)
        bindings = self._bindings(macos_desktop)

        self.mod.install_macos_window_info_helpers(bindings, ("macos_accessibility_trusted",))

        self.assertTrue(bindings["macos_accessibility_trusted"]())
        self.assertNotIn("macos_window_info_for_pid", bindings)
        self.assertEqual(captured["trusted"][0], ())


if __name__ == "__main__":
    unittest.main()
