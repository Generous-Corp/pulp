#!/usr/bin/env python3
"""No-network tests for the local-ci SSH bundle transport helpers."""

from __future__ import annotations

import json
import pathlib
import subprocess
import tempfile
import threading
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("ssh_bundle.py")


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

    def test_config_for_bundle_probe_falls_back_when_submission_config_is_unreadable(self) -> None:
        invalid = self.root / "invalid-config.json"
        invalid.write_text("{not json}\n")
        fallback = {"targets": {"fallback": {"type": "ssh"}}}

        missing_result = self.mod.config_for_bundle_probe(
            {"submission": {"config_path": str(self.root / "missing-config.json")}},
            load_config_file_fn=lambda path: json.loads(pathlib.Path(path).read_text()),
            load_optional_config_fn=lambda: fallback,
        )
        invalid_result = self.mod.config_for_bundle_probe(
            {"submission": {"config_path": str(invalid)}},
            load_config_file_fn=lambda path: json.loads(pathlib.Path(path).read_text()),
            load_optional_config_fn=lambda: fallback,
        )

        self.assertEqual(missing_result, fallback)
        self.assertEqual(invalid_result, fallback)
        self.assertEqual(invalid.read_text(), "{not json}\n")

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

    def test_sync_job_bundle_uses_probe_config_after_upload_timeout(self) -> None:
        bundle = self.root / "job124.bundle"
        bundle.write_text("bundle")
        submitted_config = {"targets": {"windows": {"type": "ssh", "host": "desktop.example.com"}}}
        submitted_path = self.root / "submission-config.json"
        submitted_path.write_text(json.dumps(submitted_config) + "\n")
        captured = {"timeouts": 0, "probe_config": None}

        class SlowProc:
            returncode = 0

            def communicate(self, timeout=None):
                if captured["timeouts"] == 0:
                    captured["timeouts"] += 1
                    raise subprocess.TimeoutExpired(["scp"], timeout)
                return ("", "")

            def terminate(self):
                self.returncode = 0

            def kill(self):
                self.returncode = -9

        def probe(_host, _remote_name, *, config):
            captured["probe_config"] = config
            return bundle.stat().st_size

        remote_name, bundle_ref = self.mod.sync_job_bundle_to_ssh_host(
            "desktop.example.com",
            {
                "id": "job124",
                "sha": "c" * 40,
                "submission": {"config_path": str(submitted_path)},
            },
            create_job_bundle_fn=lambda job: bundle,
            remote_bundle_name_fn=self.mod.remote_bundle_name,
            bundle_ref_name_fn=self.mod.bundle_ref_name,
            config_for_bundle_probe_fn=lambda job, config: self.mod.config_for_bundle_probe(
                job,
                config,
                load_config_file_fn=lambda path: json.loads(pathlib.Path(path).read_text()),
                load_optional_config_fn=lambda: {"targets": {}},
            ),
            probe_uploaded_bundle_size_fn=probe,
            now_iso_fn=lambda: "2026-06-09T00:00:00+00:00",
            popen_fn=lambda *_args, **_kwargs: SlowProc(),
            stdout_pipe=subprocess.PIPE,
            stderr_pipe=subprocess.PIPE,
            timeout_expired_type=subprocess.TimeoutExpired,
            time_fn=lambda: 100.0,
        )

        self.assertEqual(remote_name, "pulp-ci-job124.bundle")
        self.assertEqual(bundle_ref, "refs/pulp-ci-bundles/job124")
        self.assertEqual(captured["timeouts"], 1)
        self.assertEqual(captured["probe_config"]["targets"]["windows"]["host"], "desktop.example.com")

    def test_sync_job_bundle_times_out_and_keeps_local_bundle(self) -> None:
        bundle = self.root / "job125.bundle"
        bundle.write_text("bundle")
        captured = {"kills": 0, "communicates": 0}

        class HungProc:
            returncode = None

            def communicate(self, timeout=None):
                captured["communicates"] += 1
                return ("partial stdout", "partial stderr")

            def kill(self):
                captured["kills"] += 1

        with self.assertRaisesRegex(RuntimeError, "timed out waiting for scp"):
            self.mod.sync_job_bundle_to_ssh_host(
                "ubuntu",
                {"id": "job125", "sha": "e" * 40},
                create_job_bundle_fn=lambda job: bundle,
                remote_bundle_name_fn=self.mod.remote_bundle_name,
                bundle_ref_name_fn=self.mod.bundle_ref_name,
                config_for_bundle_probe_fn=lambda job, config: {"targets": {}},
                probe_uploaded_bundle_size_fn=lambda *_args, **_kwargs: None,
                now_iso_fn=lambda: "2026-06-09T00:00:00+00:00",
                popen_fn=lambda *_args, **_kwargs: HungProc(),
                stdout_pipe=subprocess.PIPE,
                stderr_pipe=subprocess.PIPE,
                timeout_expired_type=subprocess.TimeoutExpired,
                time_fn=iter([100.0, 401.0]).__next__,
            )

        self.assertEqual(captured["kills"], 1)
        self.assertEqual(captured["communicates"], 1)
        self.assertEqual(bundle.read_text(), "bundle")

    def test_sync_job_bundle_kills_after_remote_size_completion_if_terminate_hangs(self) -> None:
        bundle = self.root / "job126.bundle"
        bundle.write_text("bundle")
        captured = {"kills": 0, "terminates": 0, "communicates": 0, "probes": []}

        class SlowProc:
            returncode = None

            def communicate(self, timeout=None):
                captured["communicates"] += 1
                if captured["communicates"] in {1, 2}:
                    raise subprocess.TimeoutExpired(["scp"], timeout)
                return ("", "")

            def terminate(self):
                captured["terminates"] += 1

            def kill(self):
                captured["kills"] += 1

        def probe(host, remote_name, *, config):
            captured["probes"].append((host, remote_name, config))
            return bundle.stat().st_size

        remote_name, bundle_ref = self.mod.sync_job_bundle_to_ssh_host(
            "ubuntu",
            {"id": "job126", "sha": "f" * 40},
            config={"targets": {"ubuntu": {"type": "ssh"}}},
            create_job_bundle_fn=lambda job: bundle,
            remote_bundle_name_fn=self.mod.remote_bundle_name,
            bundle_ref_name_fn=self.mod.bundle_ref_name,
            config_for_bundle_probe_fn=lambda job, config: config or {"targets": {}},
            probe_uploaded_bundle_size_fn=probe,
            now_iso_fn=lambda: "2026-06-09T00:00:00+00:00",
            popen_fn=lambda *_args, **_kwargs: SlowProc(),
            stdout_pipe=subprocess.PIPE,
            stderr_pipe=subprocess.PIPE,
            timeout_expired_type=subprocess.TimeoutExpired,
            time_fn=lambda: 100.0,
        )

        self.assertEqual(remote_name, "pulp-ci-job126.bundle")
        self.assertEqual(bundle_ref, "refs/pulp-ci-bundles/job126")
        self.assertEqual(captured["terminates"], 1)
        self.assertEqual(captured["kills"], 1)
        self.assertEqual(captured["communicates"], 3)
        self.assertEqual(captured["probes"][0][0], "ubuntu")
        self.assertEqual(captured["probes"][0][1], "pulp-ci-job126.bundle")
        self.assertEqual(captured["probes"][0][2]["targets"]["ubuntu"]["type"], "ssh")

    def test_sync_job_bundle_reports_scp_failure_details_and_launch_errors(self) -> None:
        bundle = self.root / "job127.bundle"
        bundle.write_text("bundle")
        captured = {"runs": 0}

        class FailingProc:
            returncode = 23

            def communicate(self, timeout=None):
                captured["runs"] += 1
                return ("stdout detail", "stderr detail")

        common_kwargs = {
            "create_job_bundle_fn": lambda job: bundle,
            "remote_bundle_name_fn": self.mod.remote_bundle_name,
            "bundle_ref_name_fn": self.mod.bundle_ref_name,
            "config_for_bundle_probe_fn": lambda job, config: {"targets": {}},
            "probe_uploaded_bundle_size_fn": lambda *_args, **_kwargs: None,
            "now_iso_fn": lambda: "2026-06-09T00:00:00+00:00",
            "stdout_pipe": subprocess.PIPE,
            "stderr_pipe": subprocess.PIPE,
            "timeout_expired_type": subprocess.TimeoutExpired,
            "time_fn": lambda: 100.0,
        }

        with self.assertRaisesRegex(RuntimeError, "stderr detail"):
            self.mod.sync_job_bundle_to_ssh_host(
                "ubuntu",
                {"id": "job127", "sha": "1" * 40},
                popen_fn=lambda *_args, **_kwargs: FailingProc(),
                **common_kwargs,
            )
        with self.assertRaisesRegex(RuntimeError, "scp missing"):
            self.mod.sync_job_bundle_to_ssh_host(
                "ubuntu",
                {"id": "job127", "sha": "1" * 40},
                popen_fn=lambda *_args, **_kwargs: (_ for _ in ()).throw(OSError("scp missing")),
                **common_kwargs,
            )

        self.assertEqual(captured["runs"], 1)
        self.assertTrue(bundle.exists())
        self.assertEqual(self.mod.remote_bundle_name("job127"), "pulp-ci-job127.bundle")

    def test_create_job_bundle_reuses_existing_artifact_across_threads(self) -> None:
        bundles = self.root / "bundles"
        job = {"id": "job-concurrent", "sha": "a" * 40}
        bundle_path = bundles / "job-concurrent.bundle"
        create_calls = []

        def fake_run(cmd, *, cwd, check):
            if cmd[:3] == ["git", "bundle", "create"]:
                create_calls.append(cmd)
                bundle_path.parent.mkdir(parents=True, exist_ok=True)
                bundle_path.write_text("bundle")
            return subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

        results = []

        shared_lock = threading.Lock()

        def locked_worker():
            results.append(
                self.mod.create_job_bundle(
                    job,
                    ensure_state_dirs_fn=lambda: bundles.mkdir(parents=True, exist_ok=True),
                    bundles_dir_fn=lambda: bundles,
                    bundle_build_lock=shared_lock,
                    root=self.root,
                    run_fn=fake_run,
                )
            )

        threads = [threading.Thread(target=locked_worker) for _ in range(2)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()

        self.assertEqual(len(create_calls), 1)
        self.assertEqual(results, [bundle_path, bundle_path])
        self.assertTrue(bundle_path.exists())


if __name__ == "__main__":
    unittest.main()
