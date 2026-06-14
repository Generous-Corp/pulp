#!/usr/bin/env python3
"""Tests for SSH bundle sync dependency bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("ssh_bundle_sync_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class SshBundleSyncBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def test_exports_match_sync_helpers(self) -> None:
        self.assertEqual(self.mod.SSH_BUNDLE_SYNC_EXPORTS, ("sync_job_bundle_to_ssh_host",))
        self.assertEqual(len(self.mod.SSH_BUNDLE_SYNC_EXPORTS), len(set(self.mod.SSH_BUNDLE_SYNC_EXPORTS)))

    def test_sync_job_bundle_binds_upload_dependencies(self) -> None:
        captured = {}

        def sync_job_bundle_to_ssh_host(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return ("pulp-ci-job1.bundle", "refs/pulp-ci-bundles/job1")

        popen_fn = object()
        pipe = object()
        time_fn = object()
        bindings = {
            "_ssh_bundle": types.SimpleNamespace(sync_job_bundle_to_ssh_host=sync_job_bundle_to_ssh_host),
            "subprocess": types.SimpleNamespace(
                Popen=popen_fn,
                PIPE=pipe,
                TimeoutExpired=TimeoutError,
            ),
            "time": types.SimpleNamespace(time=time_fn),
        }
        for name in [
            "create_job_bundle",
            "remote_bundle_name",
            "bundle_ref_name",
            "config_for_bundle_probe",
            "probe_uploaded_bundle_size",
            "now_iso",
        ]:
            bindings[name] = object()

        self.assertEqual(
            self.mod.sync_job_bundle_to_ssh_host(bindings, "ubuntu", {"id": "job1"}, "progress", {"targets": {}}),
            ("pulp-ci-job1.bundle", "refs/pulp-ci-bundles/job1"),
        )

        self.assertEqual(captured["args"], ("ubuntu", {"id": "job1"}))
        self.assertEqual(captured["kwargs"]["report_progress"], "progress")
        self.assertEqual(captured["kwargs"]["config"], {"targets": {}})
        self.assertIs(captured["kwargs"]["create_job_bundle_fn"], bindings["create_job_bundle"])
        self.assertIs(captured["kwargs"]["remote_bundle_name_fn"], bindings["remote_bundle_name"])
        self.assertIs(captured["kwargs"]["bundle_ref_name_fn"], bindings["bundle_ref_name"])
        self.assertIs(captured["kwargs"]["config_for_bundle_probe_fn"], bindings["config_for_bundle_probe"])
        self.assertIs(captured["kwargs"]["probe_uploaded_bundle_size_fn"], bindings["probe_uploaded_bundle_size"])
        self.assertIs(captured["kwargs"]["now_iso_fn"], bindings["now_iso"])
        self.assertIs(captured["kwargs"]["popen_fn"], popen_fn)
        self.assertIs(captured["kwargs"]["stdout_pipe"], pipe)
        self.assertIs(captured["kwargs"]["stderr_pipe"], pipe)
        self.assertIs(captured["kwargs"]["timeout_expired_type"], TimeoutError)
        self.assertIs(captured["kwargs"]["time_fn"], time_fn)


if __name__ == "__main__":
    unittest.main()
