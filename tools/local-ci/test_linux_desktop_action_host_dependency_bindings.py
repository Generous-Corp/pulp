#!/usr/bin/env python3
"""Tests for Linux desktop action host/process dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("linux_desktop_action_host_dependency_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class LinuxDesktopActionHostDependencyBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_host_dependency_exports_match_wrappers(self) -> None:
        self.assertEqual(
            self.mod.LINUX_DESKTOP_ACTION_HOST_DEPENDENCY_EXPORTS,
            ("linux_desktop_action_host_dependencies",),
        )

    def test_host_dependencies_bind_facade_values(self) -> None:
        bindings = {
            "ensure_host_reachable": object(),
            "probe_linux_launch_backend": object(),
            "subprocess": types.SimpleNamespace(run=object()),
        }

        deps = self.mod.linux_desktop_action_host_dependencies(bindings)

        self.assertIs(deps["ensure_host_reachable_fn"], bindings["ensure_host_reachable"])
        self.assertIs(deps["probe_linux_launch_backend_fn"], bindings["probe_linux_launch_backend"])
        self.assertIs(deps["run_fn"], bindings["subprocess"].run)

    def test_host_dependency_installer_wires_named_export(self) -> None:
        bindings = {
            "ensure_host_reachable": object(),
            "probe_linux_launch_backend": object(),
            "subprocess": types.SimpleNamespace(run=object()),
        }

        self.mod.install_linux_desktop_action_host_dependency_helpers(bindings)

        self.assertIs(bindings["linux_desktop_action_host_dependencies"]()["run_fn"], bindings["subprocess"].run)

    def test_host_dependency_installer_keeps_unknown_local_fallback(self) -> None:
        bindings = {}
        self.mod.future_linux_desktop_action_host_helper = lambda _bindings: "future"

        self.mod.install_linux_desktop_action_host_dependency_helpers(
            bindings,
            ("future_linux_desktop_action_host_helper",),
        )

        self.assertEqual(bindings["future_linux_desktop_action_host_helper"](), "future")


if __name__ == "__main__":
    unittest.main()
