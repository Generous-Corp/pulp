#!/usr/bin/env python3
"""Tests for SSH bundle facade composition."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).with_name("ssh_bundle_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


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
        calls = []

        def probe_install(bindings, names):
            calls.append(("probe", names))

        def local_install(bindings, globals_obj, names):
            calls.append(("local", names))

        self.mod.install_ssh_bundle_probe_helpers = probe_install
        self.mod.install_local_helpers = local_install

        self.mod.install_ssh_bundle_helpers(
            {},
            (
                "bundle_ref_name",
                "create_job_bundle",
                "sync_job_bundle_to_ssh_host",
                "ssh_host_uses_windows_shell",
                "custom_ssh_bundle_export",
            ),
        )

        self.assertEqual(
            calls,
            [
                ("local", ("bundle_ref_name", "create_job_bundle", "sync_job_bundle_to_ssh_host")),
                ("probe", ("ssh_host_uses_windows_shell",)),
                ("local", ("custom_ssh_bundle_export",)),
            ],
        )


if __name__ == "__main__":
    unittest.main()
