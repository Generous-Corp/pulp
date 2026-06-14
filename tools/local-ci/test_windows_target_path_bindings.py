#!/usr/bin/env python3
"""Tests for Windows target path facade bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("windows_target_path_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class WindowsTargetPathBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_exports_are_named_path_helpers(self) -> None:
        expected = (
            "windows_path_join",
            "windows_default_repo_checkout_path",
            "windows_repo_path_is_unsafe",
            "update_target_repo_path",
            "windows_repo_checkout_ready",
        )

        self.assertEqual(self.mod.WINDOWS_TARGET_PATH_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))

    def test_path_wrappers_delegate_to_windows_target_module(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        windows_target = types.SimpleNamespace(
            windows_path_join=capture("join", r"C:\Pulp"),
            windows_default_repo_checkout_path=capture("default_path", r"C:\Users\dev\pulp-validate"),
            windows_repo_path_is_unsafe=capture("unsafe", False),
            update_target_repo_path=capture("update", None),
            windows_repo_checkout_ready=capture("ready", True),
        )
        bindings = {"_windows_target": windows_target}
        config = {}
        probe = {"git_found": True}

        self.assertEqual(self.mod.windows_path_join(bindings, r"C:\Users", "dev"), r"C:\Pulp")
        self.assertEqual(captured["join"][0], (r"C:\Users", "dev"))
        self.assertEqual(self.mod.windows_default_repo_checkout_path(bindings, r"C:\Users\dev"), r"C:\Users\dev\pulp-validate")
        self.assertEqual(captured["default_path"][0], (r"C:\Users\dev",))
        self.assertFalse(self.mod.windows_repo_path_is_unsafe(bindings, r"C:\Users\dev\pulp", r"C:\Users\dev"))
        self.assertEqual(captured["unsafe"][0], (r"C:\Users\dev\pulp", r"C:\Users\dev"))
        self.mod.update_target_repo_path(bindings, config, "win", r"C:\Pulp")
        self.assertEqual(captured["update"][0], (config, "win", r"C:\Pulp"))
        self.assertTrue(self.mod.windows_repo_checkout_ready(bindings, probe))
        self.assertEqual(captured["ready"][0], (probe,))

    def test_install_windows_target_path_helpers_wires_named_exports(self) -> None:
        windows_target = types.SimpleNamespace(
            windows_path_join=lambda *parts: "\\".join(parts),
            windows_repo_checkout_ready=lambda probe: True,
        )
        bindings = {"_windows_target": windows_target}

        self.mod.install_windows_target_path_helpers(bindings, ("windows_path_join", "windows_repo_checkout_ready"))

        self.assertEqual(bindings["windows_path_join"]("C:", "Pulp"), r"C:\Pulp")
        self.assertTrue(bindings["windows_repo_checkout_ready"]({"git_found": True}))


if __name__ == "__main__":
    unittest.main()
