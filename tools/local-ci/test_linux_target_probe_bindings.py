#!/usr/bin/env python3
"""Tests for Linux target probe/tooling facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("linux_target_probe_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LinuxTargetProbeBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_probe_and_tooling_bind_facade_dependencies(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        linux_target = types.SimpleNamespace(
            LINUX_REQUIRED_REMOTE_TOOLS={"git": {"required": True}},
            LINUX_OPTIONAL_REMOTE_TOOLS={"xvfb": {"required": False}},
            probe_linux_launch_backend=capture("launch", {"mode": "xvfb"}),
            probe_linux_remote_tooling=capture("tooling", {"git_found": True}),
            linux_tooling_detail=capture("detail", "git version"),
            linux_remote_tooling_ready=capture("ready", True),
        )
        bindings = {
            "_linux_target": linux_target,
            "ssh_command_result": object(),
            "LINUX_REQUIRED_REMOTE_TOOLS": {"git": {"required": True}},
        }
        probe = {"git_found": True}

        self.assertIs(self.mod.linux_required_remote_tools(bindings), linux_target.LINUX_REQUIRED_REMOTE_TOOLS)
        self.assertIs(self.mod.linux_optional_remote_tools(bindings), linux_target.LINUX_OPTIONAL_REMOTE_TOOLS)
        self.assertEqual(self.mod.probe_linux_launch_backend(bindings, "ubuntu"), {"mode": "xvfb"})
        self.assertIs(captured["launch"][1]["ssh_command_result_fn"], bindings["ssh_command_result"])
        self.assertEqual(self.mod.probe_linux_remote_tooling(bindings, "ubuntu"), {"git_found": True})
        self.assertIs(captured["tooling"][1]["ssh_command_result_fn"], bindings["ssh_command_result"])
        self.assertEqual(self.mod.linux_tooling_detail(bindings, probe, "git", missing_hint="install git"), "git version")
        self.assertEqual(captured["detail"][1], {"missing_hint": "install git"})
        self.assertTrue(self.mod.linux_remote_tooling_ready(bindings, probe))
        self.assertIs(captured["ready"][1]["required_tools"], bindings["LINUX_REQUIRED_REMOTE_TOOLS"])


if __name__ == "__main__":
    unittest.main()
