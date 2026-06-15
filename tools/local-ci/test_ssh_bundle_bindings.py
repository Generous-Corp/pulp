#!/usr/bin/env python3
"""Tests for SSH bundle facade composition."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest
from unittest import mock



def load_module():
    return load_local_ci_module("ssh_bundle_bindings.py")


class SshBundleBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_exports_compose_focused_export_groups(self) -> None:
        self.assertEqual(
            self.mod.SSH_BUNDLE_LOCAL_EXPORTS,
            (
                *self.mod.SSH_BUNDLE_NAME_EXPORTS,
                *self.mod.SSH_BUNDLE_BUILD_EXPORTS,
                *self.mod.SSH_BUNDLE_SYNC_EXPORTS,
            ),
        )
        self.assertEqual(
            self.mod.SSH_BUNDLE_EXPORTS,
            (
                *self.mod.SSH_BUNDLE_LOCAL_EXPORTS,
                *self.mod.SSH_BUNDLE_PROBE_EXPORTS,
            ),
        )
        self.assertEqual(len(self.mod.SSH_BUNDLE_EXPORTS), len(set(self.mod.SSH_BUNDLE_EXPORTS)))

    def test_install_ssh_bundle_helpers_routes_local_probe_and_unknown_exports(self) -> None:
        bindings = {}

        with (
            mock.patch.object(self.mod, "install_ssh_bundle_probe_helpers") as probe,
            mock.patch.object(self.mod, "install_local_helpers") as install_local,
        ):
            self.mod.install_ssh_bundle_helpers(
                bindings,
                (
                    "bundle_ref_name",
                    "create_job_bundle",
                    "sync_job_bundle_to_ssh_host",
                    "ssh_host_uses_windows_shell",
                    "custom_ssh_bundle_export",
                ),
            )

        probe.assert_called_once_with(bindings, ("ssh_host_uses_windows_shell",))
        install_local.assert_has_calls(
            [
                mock.call(bindings, self.mod.__dict__, ("bundle_ref_name", "create_job_bundle", "sync_job_bundle_to_ssh_host")),
                mock.call(bindings, self.mod.__dict__, ("custom_ssh_bundle_export",)),
            ]
        )


if __name__ == "__main__":
    unittest.main()
