#!/usr/bin/env python3
"""Tests for SSH uploaded bundle size probe bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("ssh_bundle_size_probe_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class SshBundleSizeProbeBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_size_probe_exports_match_wrappers(self) -> None:
        expected = ("probe_uploaded_bundle_size",)

        self.assertEqual(self.mod.SSH_BUNDLE_SIZE_PROBE_EXPORTS, expected)
        for name in expected:
            self.assertTrue(callable(getattr(self.mod, name)))

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

    def test_install_size_probe_helpers_wires_named_exports(self) -> None:
        bindings = {
            "subprocess": types.SimpleNamespace(
                run=lambda *_args, **_kwargs: types.SimpleNamespace(returncode=0, stdout="7\n")
            ),
            "ssh_host_uses_windows_shell": lambda _config, _host: False,
        }

        self.mod.install_ssh_bundle_size_probe_helpers(bindings, ("probe_uploaded_bundle_size",))

        self.assertEqual(bindings["probe_uploaded_bundle_size"]("ubuntu", "bundle.git", config={"targets": {}}), 7)


if __name__ == "__main__":
    unittest.main()
