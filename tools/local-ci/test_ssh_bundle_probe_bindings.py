#!/usr/bin/env python3
"""Tests for SSH bundle host/probe facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("ssh_bundle_probe_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class SshBundleProbeBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_probe_exports_are_composed_from_focused_groups(self) -> None:
        expected = (
            *self.mod.SSH_BUNDLE_HOST_EXPORTS,
            *self.mod.SSH_BUNDLE_SIZE_PROBE_EXPORTS,
        )

        self.assertEqual(self.mod.SSH_BUNDLE_PROBE_EXPORTS, expected)
        self.assertEqual(len(expected), len(set(expected)))
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

    def test_probe_uploaded_bundle_size_uses_platform_command_and_last_numeric_line(self) -> None:
        calls = []

        def run_fn(cmd, **kwargs):
            calls.append((cmd, kwargs))
            return types.SimpleNamespace(returncode=0, stdout="noise\n123\n")

        bindings = {
            "subprocess": types.SimpleNamespace(run=run_fn),
            "ssh_host_uses_windows_shell": lambda _config, _host: True,
        }
        self.assertEqual(self.mod.probe_uploaded_bundle_size(bindings, "win", "bundle.git", config={"targets": {}}), 123)
        self.assertIn("cmd /V:OFF", calls[-1][0][-1])
        self.assertEqual(calls[-1][1], {"capture_output": True, "text": True, "timeout": 15})

        bindings["ssh_host_uses_windows_shell"] = lambda _config, _host: False
        self.assertEqual(self.mod.probe_uploaded_bundle_size(bindings, "ubuntu", "bundle.git", config={"targets": {}}), 123)
        self.assertIn("wc -c", calls[-1][0][-1])

        bindings["subprocess"] = types.SimpleNamespace(
            run=lambda *_args, **_kwargs: types.SimpleNamespace(returncode=1, stdout="123\n")
        )
        self.assertIsNone(self.mod.probe_uploaded_bundle_size(bindings, "ubuntu", "bundle.git", config={"targets": {}}))
        bindings["subprocess"] = types.SimpleNamespace(
            run=lambda *_args, **_kwargs: types.SimpleNamespace(returncode=0, stdout="not-a-number\n")
        )
        self.assertIsNone(self.mod.probe_uploaded_bundle_size(bindings, "ubuntu", "bundle.git", config={"targets": {}}))

    def test_probe_uploaded_bundle_size_returns_none_on_timeout(self) -> None:
        class ProbeTimeout(Exception):
            pass

        def run_fn(*_args, **_kwargs):
            raise ProbeTimeout()

        bindings = {
            "subprocess": types.SimpleNamespace(run=run_fn, TimeoutExpired=ProbeTimeout),
            "ssh_host_uses_windows_shell": lambda _config, _host: False,
        }
        self.assertIsNone(
            self.mod.probe_uploaded_bundle_size(
                bindings, "ubuntu", "bundle.git", config={"targets": {}}
            )
        )

    def test_install_probe_helpers_routes_named_exports_by_group(self) -> None:
        bindings = {
            "subprocess": types.SimpleNamespace(
                run=lambda *_args, **_kwargs: types.SimpleNamespace(returncode=0, stdout="9\n")
            ),
            "ssh_host_uses_windows_shell": lambda _config, _host: False,
        }

        self.mod.install_ssh_bundle_probe_helpers(
            bindings,
            ("target_name_for_ssh_host", "probe_uploaded_bundle_size"),
        )

        self.assertEqual(
            bindings["target_name_for_ssh_host"]({"targets": {"linux": {"host": "ubuntu"}}}, "ubuntu"),
            "linux",
        )
        self.assertEqual(bindings["probe_uploaded_bundle_size"]("ubuntu", "bundle.git", config={"targets": {}}), 9)


if __name__ == "__main__":
    unittest.main()
