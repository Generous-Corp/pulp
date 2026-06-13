#!/usr/bin/env python3
"""Tests for desktop Windows repo probe dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_windows_repo_probe_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopWindowsRepoProbeBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self):
        return {"_windows_probe": types.SimpleNamespace()}

    def test_windows_repo_probe_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"ok": True}

        bindings = self._bindings()
        bindings["_windows_probe"].probe_windows_repo_checkout = runner
        for name in ["run_windows_ssh_powershell", "windows_repo_path_is_unsafe", "parse_windows_ssh_json", "ps_literal"]:
            bindings[name] = object()

        self.assertEqual(self.mod.probe_windows_repo_checkout(bindings, "win", r"C:\Pulp"), {"ok": True})
        self.assertEqual(captured["args"], ("win", r"C:\Pulp"))
        self.assertIs(captured["kwargs"]["run_windows_ssh_powershell_fn"], bindings["run_windows_ssh_powershell"])
        self.assertIs(captured["kwargs"]["windows_repo_path_is_unsafe_fn"], bindings["windows_repo_path_is_unsafe"])
        self.assertIs(captured["kwargs"]["parse_windows_ssh_json_fn"], bindings["parse_windows_ssh_json"])
        self.assertIs(captured["kwargs"]["ps_literal_fn"], bindings["ps_literal"])

    def test_windows_repo_checkout_ensure_binds_facade_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"ready": True}

        bindings = self._bindings()
        bindings["_windows_probe"].ensure_windows_remote_repo_checkout = runner
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

    def test_repo_probe_exports_and_installer_wire_named_helpers(self):
        expected = (
            "probe_windows_repo_checkout",
            "ensure_windows_remote_repo_checkout",
        )
        self.assertEqual(self.mod.DESKTOP_WINDOWS_REPO_PROBE_EXPORTS, expected)

        captured = {}

        def runner(*args, **kwargs):
            captured["probe"] = (args, kwargs)
            return {"ok": True}

        bindings = self._bindings()
        bindings["_windows_probe"].probe_windows_repo_checkout = runner
        for name in ["run_windows_ssh_powershell", "windows_repo_path_is_unsafe", "parse_windows_ssh_json", "ps_literal"]:
            bindings[name] = object()

        self.mod.install_desktop_windows_repo_probe_helpers(bindings, ("probe_windows_repo_checkout",))

        self.assertEqual(bindings["probe_windows_repo_checkout"]("win", r"C:\Pulp"), {"ok": True})
        self.assertNotIn("ensure_windows_remote_repo_checkout", bindings)
        self.assertEqual(captured["probe"][0], ("win", r"C:\Pulp"))


if __name__ == "__main__":
    unittest.main()
