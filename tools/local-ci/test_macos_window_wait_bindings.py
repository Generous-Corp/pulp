#!/usr/bin/env python3
"""Tests for macOS window wait facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("macos_window_wait_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class MacosWindowWaitBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, macos_desktop):
        return {
            "_macos_desktop": macos_desktop,
            "time": types.SimpleNamespace(time=object(), sleep=object()),
            "macos_window_info_for_pid": object(),
            "macos_window_info_for_bundle_id": object(),
            "activate_macos_bundle_id": object(),
        }

    def test_wait_exports_match_wrappers(self) -> None:
        expected = (
            "wait_for_macos_window",
            "wait_for_macos_bundle_window",
        )

        self.assertEqual(self.mod.MACOS_WINDOW_WAIT_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_wait_bindings_delegate_to_macos_desktop_module(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        time_fn = object()
        sleep_fn = object()
        macos_desktop = types.SimpleNamespace(
            wait_for_macos_window=capture("wait_pid", {"id": 7}),
            wait_for_macos_bundle_window=capture("wait_bundle", (99, {"id": 8})),
        )
        bindings = self._bindings(macos_desktop)
        bindings["time"] = types.SimpleNamespace(time=time_fn, sleep=sleep_fn)

        self.assertEqual(self.mod.wait_for_macos_window(bindings, 123, 2.0), {"id": 7})
        self.assertIs(captured["wait_pid"][1]["macos_window_info_for_pid_fn"], bindings["macos_window_info_for_pid"])
        self.assertIs(captured["wait_pid"][1]["time_fn"], time_fn)
        self.assertIs(captured["wait_pid"][1]["sleep_fn"], sleep_fn)
        self.assertEqual(self.mod.wait_for_macos_bundle_window(bindings, "com.example.demo", 2.0), (99, {"id": 8}))
        self.assertIs(captured["wait_bundle"][1]["macos_window_info_for_bundle_id_fn"], bindings["macos_window_info_for_bundle_id"])
        self.assertIs(captured["wait_bundle"][1]["activate_macos_bundle_id_fn"], bindings["activate_macos_bundle_id"])
        self.assertIs(captured["wait_bundle"][1]["time_fn"], time_fn)
        self.assertIs(captured["wait_bundle"][1]["sleep_fn"], sleep_fn)

    def test_wait_installer_wires_selected_helpers(self) -> None:
        captured = {}

        def runner(*args, **kwargs):
            captured["wait"] = (args, kwargs)
            return {"id": 7}

        macos_desktop = types.SimpleNamespace(wait_for_macos_window=runner)
        bindings = self._bindings(macos_desktop)

        self.mod.install_macos_window_wait_helpers(bindings, ("wait_for_macos_window",))

        self.assertEqual(bindings["wait_for_macos_window"](123, 2.0), {"id": 7})
        self.assertNotIn("wait_for_macos_bundle_window", bindings)
        self.assertEqual(captured["wait"][0], (123, 2.0))


if __name__ == "__main__":
    unittest.main()
