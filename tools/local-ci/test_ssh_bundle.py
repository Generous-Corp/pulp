#!/usr/bin/env python3
"""No-network tests for the local-ci SSH bundle transport helpers."""

from __future__ import annotations

import json
import pathlib
import subprocess
import tempfile
import threading
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("ssh_bundle.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class SshBundleTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_bundle_names_are_stable(self) -> None:
        self.assertEqual(self.mod.bundle_ref_name("job123"), "refs/pulp-ci-bundles/job123")
        self.assertEqual(self.mod.remote_bundle_name("job123"), "pulp-ci-job123.bundle")

    def test_create_job_bundle_builds_and_cleans_temp_ref(self) -> None:
        bundles = self.root / "bundles"
        calls = []

        def fake_run(cmd, *, cwd, check):
            calls.append((cmd, cwd, check))
            if cmd[:3] == ["git", "bundle", "create"]:
                pathlib.Path(cmd[3]).write_text("bundle")
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

        bundle = self.mod.create_job_bundle(
            {"id": "job123", "sha": "a" * 40},
            ensure_state_dirs_fn=lambda: bundles.mkdir(parents=True, exist_ok=True),
            bundles_dir_fn=lambda: bundles,
            bundle_build_lock=threading.Lock(),
            root=self.root,
            run_fn=fake_run,
        )

        self.assertEqual(bundle, bundles / "job123.bundle")
        self.assertEqual(bundle.read_text(), "bundle")
        self.assertEqual(calls[0][0], ["git", "update-ref", "refs/pulp-ci-bundles/job123", "a" * 40])
        self.assertEqual(calls[1][0][:3], ["git", "bundle", "create"])
        self.assertEqual(calls[2][0], ["git", "update-ref", "-d", "refs/pulp-ci-bundles/job123"])
        self.assertTrue(all(cwd == self.root and check for _, cwd, check in calls))

    def test_create_job_bundle_reuses_existing_artifact(self) -> None:
        bundles = self.root / "bundles"
        bundles.mkdir()
        existing = bundles / "job123.bundle"
        existing.write_text("bundle")

        bundle = self.mod.create_job_bundle(
            {"id": "job123", "sha": "a" * 40},
            ensure_state_dirs_fn=lambda: None,
            bundles_dir_fn=lambda: bundles,
            bundle_build_lock=threading.Lock(),
            root=self.root,
            run_fn=lambda *args, **kwargs: self.fail("existing bundle should be reused"),
        )

        self.assertEqual(bundle, existing)

    def test_config_for_bundle_probe_uses_explicit_submission_then_fallback(self) -> None:
        explicit = {"targets": {"ubuntu": {"type": "ssh"}}}
        self.assertIs(
            self.mod.config_for_bundle_probe(
                {},
                explicit,
                load_config_file_fn=lambda path: self.fail("explicit config should win"),
                load_optional_config_fn=lambda: None,
            ),
            explicit,
        )

        submitted_path = self.root / "config.json"
        submitted_path.write_text(json.dumps({"targets": {"windows": {"type": "ssh"}}}) + "\n")
        loaded = self.mod.config_for_bundle_probe(
            {"submission": {"config_path": str(submitted_path)}},
            load_config_file_fn=lambda path: json.loads(pathlib.Path(path).read_text()),
            load_optional_config_fn=lambda: {"targets": {"fallback": {}}},
        )
        self.assertEqual(loaded["targets"], {"windows": {"type": "ssh"}})

        fallback = self.mod.config_for_bundle_probe(
            {"submission": {"config_path": str(self.root / "missing.json")}},
            load_config_file_fn=lambda path: (_ for _ in ()).throw(FileNotFoundError(path)),
            load_optional_config_fn=lambda: {"targets": {"fallback": {}}},
        )
        self.assertEqual(fallback["targets"], {"fallback": {}})

    def test_sync_job_bundle_uploads_with_progress_and_no_network(self) -> None:
        bundle = self.root / "job123.bundle"
        bundle.write_text("bundle")
        progress = []
        captured = {}

        class FakeProc:
            returncode = 0

            def communicate(self, timeout=None):
                captured["timeout"] = timeout
                return ("", "")

        remote_name, bundle_ref = self.mod.sync_job_bundle_to_ssh_host(
            "ubuntu",
            {"id": "job123", "sha": "b" * 40},
            report_progress=lambda **event: progress.append(event),
            config={"targets": {"ubuntu": {"type": "ssh"}}},
            create_job_bundle_fn=lambda job: bundle,
            remote_bundle_name_fn=self.mod.remote_bundle_name,
            bundle_ref_name_fn=self.mod.bundle_ref_name,
            config_for_bundle_probe_fn=lambda job, config: config or {"targets": {}},
            probe_uploaded_bundle_size_fn=lambda *args, **kwargs: self.fail("probe is not needed on successful scp"),
            now_iso_fn=lambda: "2026-06-09T00:00:00+00:00",
            popen_fn=lambda cmd, **kwargs: captured.update(cmd=cmd, kwargs=kwargs) or FakeProc(),
            stdout_pipe=subprocess.PIPE,
            stderr_pipe=subprocess.PIPE,
            timeout_expired_type=subprocess.TimeoutExpired,
            time_fn=lambda: 100.0,
        )

        self.assertEqual(remote_name, "pulp-ci-job123.bundle")
        self.assertEqual(bundle_ref, "refs/pulp-ci-bundles/job123")
        self.assertEqual(captured["cmd"], ["scp", str(bundle), "ubuntu:pulp-ci-job123.bundle"])
        self.assertIs(captured["kwargs"]["stdout"], subprocess.PIPE)
        self.assertIs(captured["kwargs"]["stderr"], subprocess.PIPE)
        self.assertTrue(captured["kwargs"]["text"])
        self.assertEqual(progress[0]["phase"], "bundle-upload")
        self.assertEqual(progress[0]["transport_mode"], "bundle")


if __name__ == "__main__":
    unittest.main()
