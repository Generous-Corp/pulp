#!/usr/bin/env python3
"""Tests for desktop Windows repo probe dependency bindings."""

from module_test_utils import load_module_from_path
import types
import unittest
from unittest import mock
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_windows_repo_probe_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopWindowsRepoProbeBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self):
        return {"_windows_probe": types.SimpleNamespace()}

    def test_repo_probe_exports_and_installer_wire_named_helpers(self):
        expected = (
            *self.mod.DESKTOP_WINDOWS_REPO_CHECKOUT_PROBE_EXPORTS,
            *self.mod.DESKTOP_WINDOWS_REPO_CHECKOUT_ENSURE_EXPORTS,
        )
        self.assertEqual(self.mod.DESKTOP_WINDOWS_REPO_PROBE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

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

    def test_repo_probe_installer_routes_selected_groups_and_unknown_fallback(self):
        bindings = self._bindings()

        with (
            mock.patch.object(self.mod, "install_desktop_windows_repo_checkout_probe_helpers") as probe,
            mock.patch.object(self.mod, "install_desktop_windows_repo_checkout_ensure_helpers") as ensure,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_desktop_windows_repo_probe_helpers(
                bindings,
                ("probe_windows_repo_checkout", "ensure_windows_remote_repo_checkout", "unknown_helper"),
            )

        probe.assert_called_once_with(bindings, ("probe_windows_repo_checkout",))
        ensure.assert_called_once_with(bindings, ("ensure_windows_remote_repo_checkout",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))


if __name__ == "__main__":
    unittest.main()
