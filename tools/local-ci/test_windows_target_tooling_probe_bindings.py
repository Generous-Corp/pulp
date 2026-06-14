#!/usr/bin/env python3
"""Tests for Windows target remote-tooling probe bindings."""

from __future__ import annotations

from pathlib import Path
import types
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).with_name("windows_target_tooling_probe_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsTargetToolingProbeBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_exports_are_named_tooling_helpers(self) -> None:
        expected = (
            "windows_tooling_detail",
            "windows_remote_tooling_ready",
        )

        self.assertEqual(self.mod.WINDOWS_TARGET_TOOLING_PROBE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_tooling_wrappers_delegate_to_windows_target_module(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        windows_target = types.SimpleNamespace(
            windows_tooling_detail=capture("tool_detail", "git version"),
            windows_remote_tooling_ready=capture("ready", True),
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


if __name__ == "__main__":
    unittest.main()
