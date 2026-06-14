#!/usr/bin/env python3
"""Tests for Windows target session identity bindings."""

from __future__ import annotations

import types
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("windows_target_session_identity_bindings.py")


class WindowsTargetSessionIdentityBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_exports_are_named_identity_helpers(self) -> None:
        expected = (
            "default_windows_session_task_name",
            "desktop_target_contract",
        )

        self.assertEqual(self.mod.WINDOWS_TARGET_SESSION_IDENTITY_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_identity_wrappers_delegate_arguments(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        windows_target = types.SimpleNamespace(
            default_windows_session_task_name=capture("task", "Task-win"),
            desktop_target_contract=capture("contract", {"kind": "windows-session-agent"}),
        )
        bindings = {"_windows_target": windows_target}

        self.assertEqual(self.mod.default_windows_session_task_name(bindings, "win"), "Task-win")
        self.assertEqual(captured["task"][0], ("win",))
        self.assertEqual(
            self.mod.desktop_target_contract(bindings, "win", {"adapter": "windows-session-agent"}),
            {"kind": "windows-session-agent"},
        )
        self.assertEqual(captured["contract"][0], ("win", {"adapter": "windows-session-agent"}))


if __name__ == "__main__":
    unittest.main()
