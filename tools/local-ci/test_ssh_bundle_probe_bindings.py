#!/usr/bin/env python3
"""Tests for SSH bundle host/probe facade bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("ssh_bundle_probe_bindings.py")


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

    def test_install_probe_helpers_routes_named_exports_by_group(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_ssh_bundle_host_helpers") as install_host,
            mock.patch.object(self.mod, "install_ssh_bundle_size_probe_helpers") as install_size_probe,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_ssh_bundle_probe_helpers(
                bindings,
                ("target_name_for_ssh_host", "probe_uploaded_bundle_size", "unknown_helper"),
            )

        install_host.assert_called_once_with(bindings, ("target_name_for_ssh_host",))
        install_size_probe.assert_called_once_with(bindings, ("probe_uploaded_bundle_size",))
        install_local.assert_called_once_with(bindings, self.mod.__dict__, ("unknown_helper",))


if __name__ == "__main__":
    unittest.main()
