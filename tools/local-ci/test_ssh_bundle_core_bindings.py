#!/usr/bin/env python3
"""Tests for SSH bundle creation/sync facade bindings."""

from __future__ import annotations

from module_test_utils import load_module_from_path
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("ssh_bundle_core_bindings.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class SshBundleCoreBindingsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()

    def _bindings(self, ssh_bundle, **overrides):
        bindings = {
            "_ssh_bundle": ssh_bundle,
            "ROOT": Path("/repo"),
            "_BUNDLE_BUILD_LOCK": object(),
            "subprocess": types.SimpleNamespace(
                run=object(),
                Popen=object(),
                PIPE=object(),
                TimeoutExpired=TimeoutError,
            ),
            "time": types.SimpleNamespace(time=object()),
        }
        for name in [
            "ensure_state_dirs",
            "bundles_dir",
            "load_config_file",
            "load_optional_config",
            "create_job_bundle",
            "remote_bundle_name",
            "bundle_ref_name",
            "config_for_bundle_probe",
            "probe_uploaded_bundle_size",
            "now_iso",
        ]:
            bindings[name] = object()
        bindings.update(overrides)
        return bindings

    def test_bundle_helpers_bind_facade_dependencies(self) -> None:
        captured = {}

        def capture(name, result):
            def inner(*args, **kwargs):
                captured[name] = (args, kwargs)
                return result

            return inner

        ssh_bundle = types.SimpleNamespace(
            bundle_ref_name=capture("ref", "refs/pulp-ci-bundles/job1"),
            remote_bundle_name=capture("remote", "pulp-ci-job1.bundle"),
            create_job_bundle=capture("create", Path("/state/job1.bundle")),
            config_for_bundle_probe=capture("config", {"targets": {}}),
            sync_job_bundle_to_ssh_host=capture("sync", ("pulp-ci-job1.bundle", "refs/pulp-ci-bundles/job1")),
        )
        popen_fn = object()
        pipe = object()
        run_fn = object()
        time_fn = object()
        bindings = self._bindings(
            ssh_bundle,
            subprocess=types.SimpleNamespace(
                run=run_fn,
                Popen=popen_fn,
                PIPE=pipe,
                TimeoutExpired=TimeoutError,
            ),
            time=types.SimpleNamespace(time=time_fn),
        )

        self.assertEqual(self.mod.bundle_ref_name(bindings, "job1"), "refs/pulp-ci-bundles/job1")
        self.assertEqual(captured["ref"][0], ("job1",))
        self.assertEqual(self.mod.remote_bundle_name(bindings, "job1"), "pulp-ci-job1.bundle")
        self.assertEqual(captured["remote"][0], ("job1",))

        self.assertEqual(self.mod.create_job_bundle(bindings, {"id": "job1"}), Path("/state/job1.bundle"))
        self.assertIs(captured["create"][1]["ensure_state_dirs_fn"], bindings["ensure_state_dirs"])
        self.assertIs(captured["create"][1]["bundles_dir_fn"], bindings["bundles_dir"])
        self.assertIs(captured["create"][1]["bundle_build_lock"], bindings["_BUNDLE_BUILD_LOCK"])
        self.assertEqual(captured["create"][1]["root"], Path("/repo"))
        self.assertIs(captured["create"][1]["run_fn"], run_fn)

        self.assertEqual(self.mod.config_for_bundle_probe(bindings, {"id": "job1"}, None), {"targets": {}})
        self.assertIs(captured["config"][1]["load_config_file_fn"], bindings["load_config_file"])
        self.assertIs(captured["config"][1]["load_optional_config_fn"], bindings["load_optional_config"])

        self.assertEqual(
            self.mod.sync_job_bundle_to_ssh_host(bindings, "ubuntu", {"id": "job1"}, "progress", {"targets": {}}),
            ("pulp-ci-job1.bundle", "refs/pulp-ci-bundles/job1"),
        )
        sync_kwargs = captured["sync"][1]
        self.assertIs(sync_kwargs["create_job_bundle_fn"], bindings["create_job_bundle"])
        self.assertIs(sync_kwargs["remote_bundle_name_fn"], bindings["remote_bundle_name"])
        self.assertIs(sync_kwargs["bundle_ref_name_fn"], bindings["bundle_ref_name"])
        self.assertIs(sync_kwargs["config_for_bundle_probe_fn"], bindings["config_for_bundle_probe"])
        self.assertIs(sync_kwargs["probe_uploaded_bundle_size_fn"], bindings["probe_uploaded_bundle_size"])
        self.assertIs(sync_kwargs["now_iso_fn"], bindings["now_iso"])
        self.assertIs(sync_kwargs["popen_fn"], popen_fn)
        self.assertIs(sync_kwargs["stdout_pipe"], pipe)
        self.assertIs(sync_kwargs["stderr_pipe"], pipe)
        self.assertIs(sync_kwargs["timeout_expired_type"], TimeoutError)
        self.assertIs(sync_kwargs["time_fn"], time_fn)


if __name__ == "__main__":
    unittest.main()
