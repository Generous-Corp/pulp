#!/usr/bin/env python3
"""Tests for desktop Windows repo checkout probe bindings."""

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("desktop_windows_repo_checkout_probe_bindings.py")


class DesktopWindowsRepoCheckoutProbeBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_repo_checkout_probe_exports_match_wrappers(self):
        self.assertEqual(self.mod.DESKTOP_WINDOWS_REPO_CHECKOUT_PROBE_EXPORTS, ("probe_windows_repo_checkout",))
        self.assertTrue(callable(self.mod.probe_windows_repo_checkout))

    def test_repo_checkout_probe_binds_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"ok": True}

        bindings = {
            "_windows_probe": types.SimpleNamespace(probe_windows_repo_checkout=runner),
            "run_windows_ssh_powershell": object(),
            "windows_repo_path_is_unsafe": object(),
            "parse_windows_ssh_json": object(),
            "ps_literal": object(),
        }

        self.assertEqual(self.mod.probe_windows_repo_checkout(bindings, "win", r"C:\Pulp"), {"ok": True})
        self.assertEqual(captured["args"], ("win", r"C:\Pulp"))
        self.assertIs(captured["kwargs"]["run_windows_ssh_powershell_fn"], bindings["run_windows_ssh_powershell"])
        self.assertIs(captured["kwargs"]["windows_repo_path_is_unsafe_fn"], bindings["windows_repo_path_is_unsafe"])
        self.assertIs(captured["kwargs"]["parse_windows_ssh_json_fn"], bindings["parse_windows_ssh_json"])
        self.assertIs(captured["kwargs"]["ps_literal_fn"], bindings["ps_literal"])

    def test_repo_checkout_probe_installer_wires_named_export(self):
        bindings = {
            "_windows_probe": types.SimpleNamespace(probe_windows_repo_checkout=lambda *args, **kwargs: {"ok": True}),
            "run_windows_ssh_powershell": object(),
            "windows_repo_path_is_unsafe": object(),
            "parse_windows_ssh_json": object(),
            "ps_literal": object(),
        }

        self.mod.install_desktop_windows_repo_checkout_probe_helpers(bindings)

        self.assertEqual(bindings["probe_windows_repo_checkout"]("win", r"C:\Pulp"), {"ok": True})


if __name__ == "__main__":
    unittest.main()
