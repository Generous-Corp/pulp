#!/usr/bin/env python3
"""Tests for SSH bundle host classification bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest



def load_module():
    return load_local_ci_module("ssh_bundle_host_bindings.py")


class SshBundleHostBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_host_exports_match_wrappers(self) -> None:
        expected = (
            "target_name_for_ssh_host",
            "ssh_host_uses_windows_shell",
        )

        self.assertEqual(self.mod.SSH_BUNDLE_HOST_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

    def test_target_lookup_and_shell_detection_preserve_facade_target_name_seam(self) -> None:
        bindings = {}
        config = {
            "targets": {
                "custom": {"host": "ssh-host", "repo_path": r"C:\Pulp"},
                "ubuntu": {"host": "ubuntu", "repo_path": "/tmp/pulp"},
            }
        }

        self.assertEqual(self.mod.target_name_for_ssh_host(bindings, config, "ssh-host"), "custom")
        self.assertIsNone(self.mod.target_name_for_ssh_host(bindings, {"targets": {}}, "missing"))

        bindings["target_name_for_ssh_host"] = lambda _config, _host: "custom"
        self.assertTrue(self.mod.ssh_host_uses_windows_shell(bindings, config, "ssh-host"))
        bindings["target_name_for_ssh_host"] = lambda _config, _host: "ubuntu"
        self.assertFalse(self.mod.ssh_host_uses_windows_shell(bindings, config, "ubuntu"))
        bindings["target_name_for_ssh_host"] = lambda _config, _host: None
        self.assertTrue(self.mod.ssh_host_uses_windows_shell(bindings, config, "win-builder"))

    def test_install_host_helpers_wires_named_exports(self) -> None:
        bindings = {}

        self.mod.install_ssh_bundle_host_helpers(bindings, ("target_name_for_ssh_host",))

        self.assertEqual(
            bindings["target_name_for_ssh_host"]({"targets": {"linux": {"host": "ubuntu"}}}, "ubuntu"),
            "linux",
        )


if __name__ == "__main__":
    unittest.main()
