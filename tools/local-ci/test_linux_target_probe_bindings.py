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

    def test_exports_are_named_constant_and_probe_helpers(self) -> None:
        constant_expected = (
            "linux_required_remote_tools",
            "linux_optional_remote_tools",
        )
        probe_expected = (
            "probe_linux_launch_backend",
            "probe_linux_remote_tooling",
            "linux_tooling_detail",
            "linux_remote_tooling_ready",
        )

        self.assertEqual(self.mod.LINUX_TARGET_CONSTANT_EXPORTS, constant_expected)
        self.assertEqual(self.mod.LINUX_TARGET_PROBE_EXPORTS, probe_expected)
        self.assertEqual(len(constant_expected), len(set(constant_expected)))
        self.assertEqual(len(probe_expected), len(set(probe_expected)))

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

    def test_install_linux_target_constant_helpers_wires_named_exports(self) -> None:
        linux_target = types.SimpleNamespace(
            LINUX_REQUIRED_REMOTE_TOOLS={"git": {"required": True}},
            LINUX_OPTIONAL_REMOTE_TOOLS={"xvfb": {"required": False}},
        )
        bindings = {"_linux_target": linux_target}

        self.mod.install_linux_target_constant_helpers(bindings, ("linux_required_remote_tools",))

        self.assertIs(bindings["linux_required_remote_tools"](), linux_target.LINUX_REQUIRED_REMOTE_TOOLS)

    def test_install_linux_target_constant_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_linux_constant_helper = lambda _bindings: "future"

        self.mod.install_linux_target_constant_helpers(bindings, ("future_linux_constant_helper",))

        self.assertEqual(bindings["future_linux_constant_helper"](), "future")

    def test_install_linux_target_probe_helpers_wires_named_exports(self) -> None:
        linux_target = types.SimpleNamespace(
            linux_tooling_detail=lambda probe, tool_name, *, missing_hint=None: missing_hint,
        )
        bindings = {"_linux_target": linux_target}

        self.mod.install_linux_target_probe_helpers(bindings, ("linux_tooling_detail",))

        self.assertEqual(
            bindings["linux_tooling_detail"]({"git_found": False}, "git", missing_hint="install git"),
            "install git",
        )

    def test_install_linux_target_probe_helpers_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_linux_probe_helper = lambda _bindings: "future"

        self.mod.install_linux_target_probe_helpers(bindings, ("future_linux_probe_helper",))

        self.assertEqual(bindings["future_linux_probe_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
