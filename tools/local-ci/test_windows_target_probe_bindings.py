#!/usr/bin/env python3
"""Tests for Windows target probe/detail facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("windows_target_probe_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsTargetProbeBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_probe_wrappers_delegate_to_windows_target_module(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        windows_target = types.SimpleNamespace(
            windows_tooling_detail=capture("tool_detail", "git version"),
            windows_remote_tooling_ready=capture("ready", True),
            windows_desktop_session_user=capture("user", "dev"),
            windows_desktop_session_state=capture("state", "Active"),
            windows_repo_checkout_detail=capture("checkout_detail", r"C:\Pulp"),
        )
        bindings = {
            "_windows_target": windows_target,
            "WINDOWS_REQUIRED_REMOTE_TOOLS": {"git": {"required": True}},
        }
        probe = {"git_found": True}

        self.assertEqual(self.mod.windows_tooling_detail(bindings, probe, "git", missing_hint="install git"), "git version")
        self.assertEqual(captured["tool_detail"][0], (probe, "git"))
        self.assertEqual(captured["tool_detail"][1], {"missing_hint": "install git"})
        self.assertTrue(self.mod.windows_remote_tooling_ready(bindings, probe))
        self.assertEqual(captured["ready"][0], (probe,))
        self.assertIs(captured["ready"][1]["required_tools"], bindings["WINDOWS_REQUIRED_REMOTE_TOOLS"])
        self.assertEqual(self.mod.windows_desktop_session_user(bindings, probe), "dev")
        self.assertEqual(captured["user"][0], (probe,))
        self.assertEqual(self.mod.windows_desktop_session_state(bindings, probe), "Active")
        self.assertEqual(captured["state"][0], (probe,))
        self.assertEqual(self.mod.windows_repo_checkout_detail(bindings, probe, fallback_path=r"C:\Fallback"), r"C:\Pulp")
        self.assertEqual(captured["checkout_detail"][0], (probe,))
        self.assertEqual(captured["checkout_detail"][1], {"fallback_path": r"C:\Fallback"})


if __name__ == "__main__":
    unittest.main()
