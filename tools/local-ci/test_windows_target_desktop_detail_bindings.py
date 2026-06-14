#!/usr/bin/env python3
"""Tests for Windows target desktop-session and checkout detail bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("windows_target_desktop_detail_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsTargetDesktopDetailBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_exports_are_named_desktop_detail_helpers(self) -> None:
        expected = (
            "windows_desktop_session_user",
            "windows_desktop_session_state",
            "windows_repo_checkout_detail",
        )

        self.assertEqual(self.mod.WINDOWS_TARGET_DESKTOP_DETAIL_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_desktop_detail_wrappers_delegate_to_windows_target_module(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        windows_target = types.SimpleNamespace(
            windows_desktop_session_user=capture("user", "dev"),
            windows_desktop_session_state=capture("state", "Active"),
            windows_repo_checkout_detail=capture("checkout_detail", r"C:\Pulp"),
        )
        bindings = {"_windows_target": windows_target}
        probe = {"git_found": True}

        self.assertEqual(self.mod.windows_desktop_session_user(bindings, probe), "dev")
        self.assertEqual(captured["user"][0], (probe,))
        self.assertEqual(self.mod.windows_desktop_session_state(bindings, probe), "Active")
        self.assertEqual(captured["state"][0], (probe,))
        self.assertEqual(self.mod.windows_repo_checkout_detail(bindings, probe, fallback_path=r"C:\Fallback"), r"C:\Pulp")
        self.assertEqual(captured["checkout_detail"][0], (probe,))
        self.assertEqual(captured["checkout_detail"][1], {"fallback_path": r"C:\Fallback"})


if __name__ == "__main__":
    unittest.main()
