#!/usr/bin/env python3
"""Tests for desktop Windows repo checkout ensure bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_windows_repo_checkout_ensure_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopWindowsRepoCheckoutEnsureBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self):
        bindings = {
            "_windows_probe": types.SimpleNamespace(),
        }
        for name in [
            "probe_windows_repo_checkout",
            "windows_repo_path_is_unsafe",
            "windows_default_repo_checkout_path",
            "run_windows_ssh_powershell",
            "parse_windows_ssh_json",
            "windows_contract_expand_expression",
            "ps_literal",
        ]:
            bindings[name] = object()
        return bindings

    def test_repo_checkout_ensure_exports_match_wrappers(self):
        self.assertEqual(
            self.mod.DESKTOP_WINDOWS_REPO_CHECKOUT_ENSURE_EXPORTS,
            ("ensure_windows_remote_repo_checkout",),
        )
        self.assertTrue(callable(self.mod.ensure_windows_remote_repo_checkout))

    def test_repo_checkout_ensure_binds_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"ready": True}

        bindings = self._bindings()
        bindings["_windows_probe"].ensure_windows_remote_repo_checkout = runner

        result = self.mod.ensure_windows_remote_repo_checkout(
            bindings,
            "win",
            r"C:\Pulp",
            remote_url="https://example/repo.git",
            bundle_name="bundle",
            bundle_ref="refs/bundle",
        )

        self.assertEqual(result, {"ready": True})
        self.assertEqual(captured["args"], ("win", r"C:\Pulp"))
        self.assertEqual(captured["kwargs"]["remote_url"], "https://example/repo.git")
        self.assertEqual(captured["kwargs"]["bundle_name"], "bundle")
        self.assertEqual(captured["kwargs"]["bundle_ref"], "refs/bundle")
        for name in [
            "probe_windows_repo_checkout",
            "windows_repo_path_is_unsafe",
            "windows_default_repo_checkout_path",
            "run_windows_ssh_powershell",
            "parse_windows_ssh_json",
            "windows_contract_expand_expression",
            "ps_literal",
        ]:
            self.assertIs(captured["kwargs"][f"{name}_fn"], bindings[name])

    def test_repo_checkout_ensure_installer_wires_named_export(self):
        bindings = self._bindings()
        bindings["_windows_probe"].ensure_windows_remote_repo_checkout = lambda *args, **kwargs: {"ready": True}

        self.mod.install_desktop_windows_repo_checkout_ensure_helpers(bindings)

        self.assertEqual(bindings["ensure_windows_remote_repo_checkout"]("win", r"C:\Pulp"), {"ready": True})


if __name__ == "__main__":
    unittest.main()
