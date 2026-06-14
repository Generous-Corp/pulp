#!/usr/bin/env python3
"""Tests for SSH bundle name dependency bindings."""

from __future__ import annotations

from module_test_utils import load_local_ci_module
import types
import unittest



def load_module():
    return load_local_ci_module("ssh_bundle_name_bindings.py")


class SshBundleNameBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_exports_match_name_helpers(self) -> None:
        self.assertEqual(
            self.mod.SSH_BUNDLE_NAME_EXPORTS,
            (
                "bundle_ref_name",
                "remote_bundle_name",
            ),
        )
        self.assertEqual(len(self.mod.SSH_BUNDLE_NAME_EXPORTS), len(set(self.mod.SSH_BUNDLE_NAME_EXPORTS)))

    def test_bundle_names_delegate_to_ssh_bundle_module(self) -> None:
        captured = {}

        def bundle_ref_name(job_id):
            captured["ref"] = job_id
            return f"refs/pulp-ci-bundles/{job_id}"

        def remote_bundle_name(job_id):
            captured["remote"] = job_id
            return f"pulp-ci-{job_id}.bundle"

        bindings = {
            "_ssh_bundle": types.SimpleNamespace(
                bundle_ref_name=bundle_ref_name,
                remote_bundle_name=remote_bundle_name,
            )
        }

        self.assertEqual(self.mod.bundle_ref_name(bindings, "job1"), "refs/pulp-ci-bundles/job1")
        self.assertEqual(self.mod.remote_bundle_name(bindings, "job1"), "pulp-ci-job1.bundle")
        self.assertEqual(captured, {"ref": "job1", "remote": "job1"})


if __name__ == "__main__":
    unittest.main()
