#!/usr/bin/env python3
"""Tests for SSH bundle facade bindings."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import types
import unittest


MODULE_PATH = Path(__file__).with_name("ssh_bundle_bindings.py")


def load_module():
    spec = importlib.util.spec_from_file_location("ssh_bundle_bindings_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class SshBundleBindingsTests(unittest.TestCase):
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
            "target_name_for_ssh_host",
            "ssh_host_uses_windows_shell",
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

    def test_target_lookup_and_shell_detection_preserve_facade_target_name_seam(self) -> None:
        bindings = self._bindings(types.SimpleNamespace())
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

        bindings = self._bindings(
            types.SimpleNamespace(),
            subprocess=types.SimpleNamespace(run=run_fn),
            ssh_host_uses_windows_shell=lambda _config, _host: True,
        )
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


if __name__ == "__main__":
    unittest.main()
