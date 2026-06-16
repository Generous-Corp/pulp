#!/usr/bin/env python3
"""Tests for SSH bundle core compatibility facade bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import unittest





def load_module():
    return load_local_ci_module("ssh_bundle_core_bindings.py")


class SshBundleCoreBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_core_facade_reexports_focused_bundle_bindings(self) -> None:
        self.assertEqual(
            self.mod.SSH_BUNDLE_CORE_EXPORTS,
            (
                *self.mod.SSH_BUNDLE_NAME_EXPORTS,
                *self.mod.SSH_BUNDLE_BUILD_EXPORTS,
                *self.mod.SSH_BUNDLE_SYNC_EXPORTS,
            ),
        )
        self.assertEqual(len(self.mod.SSH_BUNDLE_CORE_EXPORTS), len(set(self.mod.SSH_BUNDLE_CORE_EXPORTS)))

        self.assertEqual(self.mod.bundle_ref_name.__module__, "ssh_bundle_name_bindings")
        self.assertEqual(self.mod.remote_bundle_name.__module__, "ssh_bundle_name_bindings")
        self.assertEqual(self.mod.create_job_bundle.__module__, "ssh_bundle_build_bindings")
        self.assertEqual(self.mod.config_for_bundle_probe.__module__, "ssh_bundle_build_bindings")
        self.assertEqual(self.mod.sync_job_bundle_to_ssh_host.__module__, "ssh_bundle_sync_bindings")


if __name__ == "__main__":
    unittest.main()
