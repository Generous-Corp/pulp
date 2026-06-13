#!/usr/bin/env python3
"""Tests for macOS window action facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("macos_window_action_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class MacosWindowActionBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_activation_click_termination_and_quit_bind_facade_dependencies(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        run_fn = object()
        macos_desktop = types.SimpleNamespace(
            activate_macos_pid=capture("activate_pid", {"activated": True}),
            activate_macos_bundle_id=capture("activate_bundle", {"activated": True}),
            dispatch_macos_click=capture("click", {"clicked": True}),
            terminate_process=capture("terminate", None),
            quit_macos_bundle_id=capture("quit", None),
        )
        bindings = {
            "_macos_desktop": macos_desktop,
            "subprocess": types.SimpleNamespace(run=run_fn),
            "macos_window_probe_path": object(),
        }
        proc = object()

        self.assertEqual(self.mod.activate_macos_pid(bindings, 123), {"activated": True})
        self.assertIs(captured["activate_pid"][1]["probe_path_fn"], bindings["macos_window_probe_path"])
        self.assertEqual(self.mod.activate_macos_bundle_id(bindings, "com.example.demo"), {"activated": True})
        self.assertIs(captured["activate_bundle"][1]["run_fn"], run_fn)
        self.assertEqual(self.mod.dispatch_macos_click(bindings, 10.0, 20.0), {"clicked": True})
        self.assertIs(captured["click"][1]["probe_path_fn"], bindings["macos_window_probe_path"])
        self.mod.terminate_process(bindings, proc, timeout_secs=1.25)
        self.assertEqual(captured["terminate"][0], (proc,))
        self.assertEqual(captured["terminate"][1], {"timeout_secs": 1.25})
        self.mod.quit_macos_bundle_id(bindings, "com.example.demo")
        self.assertIs(captured["quit"][1]["run_fn"], run_fn)

    def test_action_exports_and_installer_wire_named_helpers(self) -> None:
        expected = (
            *self.mod.MACOS_WINDOW_ACTIVATION_EXPORTS,
            *self.mod.MACOS_WINDOW_CLICK_EXPORTS,
            *self.mod.MACOS_WINDOW_PROCESS_EXPORTS,
        )
        self.assertEqual(self.mod.MACOS_WINDOW_ACTION_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

        macos_desktop = types.SimpleNamespace(
            activate_macos_bundle_id=lambda bundle_id, **kwargs: {"bundle_id": bundle_id},
            terminate_process=lambda proc, timeout_secs=5.0: None,
        )
        bindings = {
            "_macos_desktop": macos_desktop,
            "subprocess": types.SimpleNamespace(run=object()),
        }

        self.mod.install_macos_window_action_helpers(bindings, ("activate_macos_bundle_id", "terminate_process"))

        self.assertEqual(bindings["activate_macos_bundle_id"]("com.example.demo"), {"bundle_id": "com.example.demo"})
        self.assertIsNone(bindings["terminate_process"](object()))
        self.assertNotIn("dispatch_macos_click", bindings)

    def test_action_installer_routes_selected_groups(self) -> None:
        macos_desktop = types.SimpleNamespace(
            activate_macos_bundle_id=lambda bundle_id, **kwargs: {"bundle_id": bundle_id},
            dispatch_macos_click=lambda *args, **kwargs: {"clicked": True},
            quit_macos_bundle_id=lambda bundle_id, **kwargs: None,
        )
        bindings = {
            "_macos_desktop": macos_desktop,
            "subprocess": types.SimpleNamespace(run=object()),
            "macos_window_probe_path": object(),
        }

        self.mod.install_macos_window_action_helpers(
            bindings,
            ("activate_macos_bundle_id", "dispatch_macos_click", "quit_macos_bundle_id"),
        )

        self.assertEqual(bindings["activate_macos_bundle_id"]("com.example.demo"), {"bundle_id": "com.example.demo"})
        self.assertEqual(bindings["dispatch_macos_click"](10.0, 20.0), {"clicked": True})
        self.assertIsNone(bindings["quit_macos_bundle_id"]("com.example.demo"))
        self.assertNotIn("activate_macos_pid", bindings)
        self.assertNotIn("terminate_process", bindings)


if __name__ == "__main__":
    unittest.main()
