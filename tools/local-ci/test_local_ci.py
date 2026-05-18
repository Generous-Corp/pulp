#!/usr/bin/env python3

import io
import importlib.util
import json
import os
import subprocess
import tempfile
import threading
import unittest
from urllib.parse import urlparse
from unittest import mock
from contextlib import redirect_stdout
from datetime import datetime, timezone
from pathlib import Path
from types import SimpleNamespace


MODULE_PATH = Path(__file__).with_name("local_ci.py")
VALIDATE_BUILD_PATH = MODULE_PATH.parent.parent.parent / "validate-build.sh"


def load_module():
    spec = importlib.util.spec_from_file_location("pulp_local_ci", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class LocalCiTests(unittest.TestCase):
    def _set_target_enabled(self, name: str, enabled: bool):
        payload = json.loads(self.config_path.read_text())
        payload.setdefault("targets", {}).setdefault(name, {})["enabled"] = enabled
        self.config_path.write_text(json.dumps(payload) + "\n")

    def _write_desktop_manifest(self, config, target, action, manifest):
        bundle = self.mod.create_desktop_run_bundle(config, target, action)
        payload = dict(manifest)
        artifacts = dict(payload.get("artifacts", {}))
        artifacts.setdefault("bundle_dir", str(bundle))
        payload["artifacts"] = artifacts
        (bundle / "manifest.json").write_text(json.dumps(payload) + "\n")
        return bundle, payload

    def setUp(self):
        self.tmpdir = tempfile.TemporaryDirectory()
        root = Path(self.tmpdir.name)
        self.state_dir = root / "state"
        self.config_path = root / "config.json"
        self.config_path.write_text(
            json.dumps(
                {
                    "desktop_automation": {
                        "artifact_root": str(root / "desktop-artifacts"),
                    },
                    "targets": {
                        "mac": {"type": "local", "enabled": True},
                        "ubuntu": {"type": "ssh", "enabled": True, "host": "ubuntu", "repo_path": "/tmp/pulp"},
                        "windows": {"type": "ssh", "enabled": False, "host": "win2", "repo_path": "C:\\Pulp"},
                    },
                    "github_actions": {
                        "repository": "danielraffel/pulp",
                        "defaults": {
                            "workflow": "build",
                            "provider": "github-hosted",
                            "wait_poll_secs": 5,
                            "match_timeout_secs": 30,
                        },
                        "workflows": {
                            "build": {
                                "providers": {
                                    "namespace": {
                                        "linux_runner_selector_json": "\"namespace-profile-default\"",
                                        "windows_runner_selector_json": "\"namespace-profile-default\"",
                                    }
                                }
                            },
                            "docs-check": {
                                "providers": {
                                    "namespace": {
                                        "runner_selector_json": "\"namespace-profile-default\""
                                    }
                                }
                            }
                        },
                    },
                    "defaults": {
                        "priority": "normal",
                        "targets": ["mac"],
                    },
                }
            )
            + "\n"
        )

        self.prev_home = os.environ.get("PULP_LOCAL_CI_HOME")
        self.prev_config = os.environ.get("PULP_LOCAL_CI_CONFIG")
        os.environ["PULP_LOCAL_CI_HOME"] = str(self.state_dir)
        os.environ["PULP_LOCAL_CI_CONFIG"] = str(self.config_path)
        self.mod = load_module()

    def tearDown(self):
        if self.prev_home is None:
            os.environ.pop("PULP_LOCAL_CI_HOME", None)
        else:
            os.environ["PULP_LOCAL_CI_HOME"] = self.prev_home

        if self.prev_config is None:
            os.environ.pop("PULP_LOCAL_CI_CONFIG", None)
        else:
            os.environ["PULP_LOCAL_CI_CONFIG"] = self.prev_config

        self.tmpdir.cleanup()

    def test_enqueue_deduplicates_and_raises_priority(self):
        first, created_first = self.mod.enqueue_job(
            "feature/test",
            "a" * 40,
            "low",
            ["mac"],
            "run",
            "full",
        )
        second, created_second = self.mod.enqueue_job(
            "feature/test",
            "a" * 40,
            "high",
            ["mac"],
            "run",
            "full",
        )

        self.assertTrue(created_first)
        self.assertFalse(created_second)
        self.assertEqual(first["id"], second["id"])

        stored = self.mod.load_job(first["id"])
        self.assertIsNotNone(stored)
        self.assertEqual(stored["priority"], "high")

    def test_desktop_target_overrides_replace_host_and_repo_path(self):
        payload = json.loads(self.config_path.read_text())
        payload.setdefault("desktop_automation", {}).setdefault("targets", {}).setdefault("windows", {}).update(
            {
                "enabled": True,
                "host": "win",
                "repo_path": r"C:\Users\daniel\Code\pulp-validate",
            }
        )
        self.config_path.write_text(json.dumps(payload) + "\n")

        config = self.mod.load_config()
        target = self.mod.resolve_desktop_target(config, "windows")

        self.assertEqual(target["host"], "win")
        self.assertEqual(target["repo_path"], r"C:\Users\daniel\Code\pulp-validate")

    def test_enqueue_treats_smoke_and_full_as_distinct_jobs(self):
        smoke_job, smoke_created = self.mod.enqueue_job(
            "feature/test",
            "a" * 40,
            "normal",
            ["mac"],
            "run",
            "smoke",
        )
        full_job, full_created = self.mod.enqueue_job(
            "feature/test",
            "a" * 40,
            "normal",
            ["mac"],
            "run",
            "full",
        )

        self.assertTrue(smoke_created)
        self.assertTrue(full_created)
        self.assertNotEqual(smoke_job["id"], full_job["id"])
        self.assertEqual(smoke_job["validation"], "smoke")
        self.assertEqual(full_job["validation"], "full")

    def test_enqueue_supersedes_older_pending_same_scope(self):
        older_job, older_created = self.mod.enqueue_job(
            "feature/test",
            "a" * 40,
            "normal",
            ["mac"],
            "run",
            "full",
        )
        newer_job, newer_created = self.mod.enqueue_job(
            "feature/test",
            "b" * 40,
            "normal",
            ["mac"],
            "run",
            "full",
        )

        self.assertTrue(older_created)
        self.assertTrue(newer_created)

        queue = self.mod.load_queue()
        older_stored = next(job for job in queue if job["id"] == older_job["id"])
        newer_stored = next(job for job in queue if job["id"] == newer_job["id"])

        self.assertEqual(older_stored["status"], "completed")
        self.assertEqual(older_stored["overall"], "superseded")
        self.assertEqual(older_stored["superseded_by"], newer_job["id"])
        self.assertEqual(newer_stored["status"], "pending")

        result = self.mod.load_result(Path(older_stored["result_file"]))
        self.assertEqual(result["overall"], "superseded")
        self.assertEqual(result["superseded_by"], newer_job["id"])

    def test_enqueue_supersedes_broader_pending_same_sha_with_narrower_scope(self):
        broader_job, broader_created = self.mod.enqueue_job(
            "feature/test",
            "a" * 40,
            "normal",
            ["mac", "windows"],
            "run",
            "smoke",
        )
        narrower_job, narrower_created = self.mod.enqueue_job(
            "feature/test",
            "a" * 40,
            "normal",
            ["windows"],
            "run",
            "smoke",
        )

        self.assertTrue(broader_created)
        self.assertTrue(narrower_created)

        queue = self.mod.load_queue()
        broader_stored = next(job for job in queue if job["id"] == broader_job["id"])
        narrower_stored = next(job for job in queue if job["id"] == narrower_job["id"])

        self.assertEqual(broader_stored["status"], "completed")
        self.assertEqual(broader_stored["overall"], "superseded")
        self.assertEqual(broader_stored["superseded_by"], narrower_job["id"])
        self.assertEqual(broader_stored["superseded_reason"], "narrower_scope_queued")
        self.assertEqual(narrower_stored["status"], "pending")

    def test_claim_next_job_prefers_higher_priority(self):
        low_job, _ = self.mod.enqueue_job("feature/low", "1" * 40, "low", ["mac"], "run", "full")
        high_job, _ = self.mod.enqueue_job("feature/high", "2" * 40, "high", ["mac"], "run", "full")

        claimed = self.mod.claim_next_job()
        self.assertIsNotNone(claimed)
        self.assertEqual(claimed["id"], high_job["id"])
        self.assertNotEqual(claimed["id"], low_job["id"])

    def test_cancel_pending_job_marks_it_completed_with_canceled_result(self):
        job, created = self.mod.enqueue_job("feature/cancel", "5" * 40, "normal", ["ubuntu"], "run", "full")
        self.assertTrue(created)

        exit_code = self.mod.cmd_cancel(SimpleNamespace(job=job["id"]))
        self.assertEqual(exit_code, 0)

        stored = self.mod.load_job(job["id"])
        self.assertIsNotNone(stored)
        self.assertEqual(stored["status"], "completed")
        self.assertEqual(stored["overall"], "canceled")
        result = self.mod.load_result(Path(stored["result_file"]))
        self.assertEqual(result["overall"], "canceled")
        self.assertEqual(result["canceled_reason"], "operator_canceled")

    def test_resolve_targets_uses_defaults_and_rejects_disabled_targets(self):
        config = self.mod.load_config()
        self.assertEqual(self.mod.resolve_targets(config, None), ["mac"])

        with self.assertRaises(ValueError):
            self.mod.resolve_targets(config, ["windows"])

    def test_config_path_prefers_shared_state_config(self):
        original_override = os.environ.pop("PULP_LOCAL_CI_CONFIG", None)
        shared_config = self.state_dir / "config.json"
        shared_config.parent.mkdir(parents=True, exist_ok=True)
        shared_config.write_text(
            json.dumps(
                {
                    "targets": {"mac": {"type": "local", "enabled": True}},
                    "defaults": {"priority": "normal", "targets": ["mac"]},
                }
            )
            + "\n"
        )
        try:
            self.assertEqual(self.mod.config_path(), shared_config)
        finally:
            if original_override is None:
                os.environ.pop("PULP_LOCAL_CI_CONFIG", None)
            else:
                os.environ["PULP_LOCAL_CI_CONFIG"] = original_override

    def test_size_helpers_report_files_and_local_ci_state_footprint(self):
        self.assertEqual(self.mod.format_size_bytes(None), "")
        self.assertEqual(self.mod.format_size_bytes(""), "")
        self.assertEqual(self.mod.format_size_bytes(512), "512 B")
        self.assertEqual(self.mod.format_size_bytes(1536), "1.5 KB")
        self.assertEqual(self.mod.path_size_bytes(self.state_dir / "missing"), 0)

        bundle_dir = self.state_dir / "bundles"
        bundle_dir.mkdir(parents=True)
        (bundle_dir / "a.bundle").write_bytes(b"abc")
        nested = bundle_dir / "nested"
        nested.mkdir()
        (nested / "b.bundle").write_bytes(b"defg")

        self.assertEqual(self.mod.path_size_bytes(bundle_dir), 7)
        footprint = self.mod.local_ci_state_footprint()
        self.assertEqual(footprint["entries"]["bundles"]["size_bytes"], 7)
        self.assertEqual(footprint["total_bytes"], 7)
        self.assertEqual(self.mod.describe_path_for_cleanup(bundle_dir), "bundles")
        outside = Path(self.tmpdir.name) / "outside"
        self.assertEqual(self.mod.describe_path_for_cleanup(outside), str(outside))

    def test_text_file_helpers_tail_trim_and_atomic_write(self):
        log_path = self.state_dir / "logs" / "job" / "mac.log"
        log_path.parent.mkdir(parents=True)
        log_path.write_text("one\ntwo\nthree\n")

        self.assertEqual(self.mod.tail_lines(self.state_dir / "missing.log"), [])
        self.assertEqual(self.mod.tail_lines(log_path, limit=2), ["two\n", "three\n"])

        target_path = self.state_dir / "results" / "result.txt"
        self.mod.atomic_write_text(target_path, "first\n")
        self.mod.atomic_write_text(target_path, "second\n")
        self.assertEqual(target_path.read_text(), "second\n")

        trimmed = self.mod.trim_line("  " + ("x" * 170) + "tail  ", max_len=12)
        self.assertEqual(len(trimmed), 12)
        self.assertTrue(trimmed.endswith("tail"))

    def test_priority_and_target_helpers_normalize_inputs(self):
        self.assertIsNone(self.mod.parse_targets_arg(""))
        self.assertEqual(
            self.mod.parse_targets_arg(" windows,mac,windows, ,ubuntu "),
            ["mac", "ubuntu", "windows"],
        )
        self.assertEqual(self.mod.normalize_priority(" HIGH "), "high")
        self.assertEqual(self.mod.priority_value(None), 50)
        with self.assertRaisesRegex(ValueError, "Invalid priority"):
            self.mod.normalize_priority("urgent")

        config = {
            "targets": {
                "mac": {"enabled": True},
                "ubuntu": {"enabled": True},
                "windows": {"enabled": True},
            },
            "defaults": {"targets": "ubuntu,mac,ubuntu"},
        }
        self.assertEqual(self.mod.resolve_targets(config, None), ["mac", "ubuntu"])
        self.assertEqual(self.mod.resolve_targets(config, ["windows", "mac", "windows"]), ["mac", "windows"])

    def test_extracted_target_helpers_cover_empty_defaults_unknown_and_disabled(self):
        config = {
            "targets": {
                "mac": {"enabled": True},
                "linux": {"enabled": False},
                "windows": {},
            },
            "defaults": {"targets": []},
        }

        self.assertEqual(self.mod.enabled_targets(config), ["mac", "windows"])
        self.assertEqual(self.mod.resolve_targets(config, None), [])

        with self.assertRaisesRegex(ValueError, "Unknown target\\(s\\): ios"):
            self.mod.resolve_targets(config, ["ios"])

        with self.assertRaisesRegex(ValueError, "Requested target\\(s\\) disabled"):
            self.mod.resolve_targets(config, ["linux"])

    def test_extracted_provenance_helpers_normalize_result_and_summarize(self):
        direct = self.mod.normalize_provenance({"direct_backend": ""})
        self.assertEqual(direct["execution_kind"], "direct")
        self.assertEqual(direct["direct_backend"], "")
        self.assertEqual(self.mod.provenance_summary(direct), "direct via local-ci")

        hosted = self.mod.normalize_provenance(
            {
                "execution_kind": "hosted",
                "hosted_orchestrator": "github-actions",
                "runner_provider": "github-hosted",
                "runner_selector": "macos-15",
                "run_id": "12345",
            }
        )
        self.assertEqual(
            self.mod.provenance_summary(hosted),
            "hosted via github-actions/github-hosted selector=macos-15 run=12345",
        )

        result = self.mod.normalize_result({"submission": {"provenance": hosted}})
        self.assertEqual(result["provenance"]["run_id"], "12345")

    def test_extracted_queue_helpers_accept_legacy_array_and_enveloped_queue(self):
        legacy_job = {
            "branch": "feature/local-ci",
            "sha": "abc123",
            "queued_at": "2026-05-18T00:00:00Z",
            "priority": " HIGH ",
            "targets": ["windows", "mac", "mac"],
            "validation": " smoke ",
            "submission": {
                "provenance": {
                    "execution_kind": "hosted",
                    "hosted_orchestrator": "github-actions",
                    "runner_provider": "github-hosted",
                }
            },
        }

        normalized = self.mod.normalize_job(legacy_job)
        self.assertEqual(len(normalized["id"]), 12)
        self.assertEqual(normalized["priority"], "high")
        self.assertEqual(normalized["targets"], ["mac", "windows"])
        self.assertEqual(normalized["validation"], "smoke")
        self.assertEqual(normalized["status"], "pending")
        self.assertEqual(normalized["provenance"]["execution_kind"], "hosted")

        queue_file = self.mod.queue_path()
        queue_file.parent.mkdir(parents=True, exist_ok=True)
        queue_file.write_text(json.dumps([legacy_job]) + "\n")
        self.assertEqual(self.mod.load_queue_unlocked()[0]["targets"], ["mac", "windows"])

        queue_file.write_text(json.dumps({"jobs": [legacy_job]}) + "\n")
        self.assertEqual(self.mod.load_queue_unlocked()[0]["priority"], "high")

        self.mod.save_queue_unlocked([normalized])
        self.assertEqual(json.loads(queue_file.read_text())[0]["id"], normalized["id"])

    def test_extracted_github_workflow_helpers_resolve_sources_and_cli_overrides(self):
        config = {
            "github_actions": {
                "defaults": {"provider": "namespace"},
                "workflows": {
                    "build": {
                        "providers": {
                            "namespace": {
                                "linux_runner_selector_json": '["ns-linux"]',
                            }
                        }
                    }
                },
            }
        }
        repo_vars = {
            "PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON": '"ns-windows"',
        }

        defaults, sources = self.mod.resolve_workflow_dispatch_defaults(
            config,
            repo_vars,
            "build",
            "namespace",
            ["linux_runner_selector_json", "windows_runner_selector_json"],
        )
        self.assertEqual(defaults["linux_runner_selector_json"], '["ns-linux"]')
        self.assertEqual(defaults["windows_runner_selector_json"], '"ns-windows"')
        self.assertIn("config github_actions.workflows.build", sources["linux_runner_selector_json"])
        self.assertIn("repo variable", sources["windows_runner_selector_json"])

        args = SimpleNamespace(
            linux_runner_selector_json='"ubuntu-24.04"',
            windows_runner_selector_json=None,
            macos_runner_selector_json=None,
        )
        self.assertEqual(
            self.mod.resolve_cli_dispatch_field_values(args, ["linux_runner_selector_json"]),
            {"linux_runner_selector_json": '"ubuntu-24.04"'},
        )

        with self.assertRaisesRegex(ValueError, "not supported for this workflow"):
            self.mod.resolve_cli_dispatch_field_values(args, ["macos_runner_selector_json"])

        with self.assertRaisesRegex(ValueError, "Unknown workflow"):
            self.mod.resolve_default_provider_for_workflow(
                {"provider": "github-hosted"}, "missing"
            )

    def test_windows_checkout_path_helpers_join_and_detect_unsafe_roots(self):
        self.assertEqual(
            self.mod.windows_path_join(r"C:\Users\daniel\\", r"\Code\\", "pulp"),
            r"C:\Users\daniel\Code\pulp",
        )
        self.assertEqual(self.mod.windows_default_repo_checkout_path(None), "pulp-validate")
        self.assertEqual(
            self.mod.windows_default_repo_checkout_path(r"C:\Users\daniel"),
            r"C:\Users\daniel\pulp-validate",
        )
        self.assertTrue(self.mod.windows_repo_path_is_unsafe(None))
        self.assertTrue(self.mod.windows_repo_path_is_unsafe(r"C:\\"))
        self.assertTrue(
            self.mod.windows_repo_path_is_unsafe(
                r"C:\Users\daniel",
                r"C:\Users\daniel",
            )
        )
        self.assertFalse(
            self.mod.windows_repo_path_is_unsafe(
                r"C:\Users\daniel\pulp-validate",
                r"C:\Users\daniel",
            )
        )

    def test_image_change_summary_falls_back_to_hash_for_non_images(self):
        before_path = Path(self.tmpdir.name) / "before.bin"
        after_path = Path(self.tmpdir.name) / "after.bin"
        diff_path = Path(self.tmpdir.name) / "diff" / "image.png"
        before_path.write_bytes(b"same")
        after_path.write_bytes(b"same")

        unchanged = self.mod.image_change_summary(before_path, after_path, diff_output_path=diff_path)
        self.assertFalse(unchanged["changed"])
        self.assertEqual(unchanged["method"], "file-hash")
        self.assertFalse(diff_path.exists())

        after_path.write_bytes(b"different")
        changed = self.mod.image_change_summary(before_path, after_path, diff_output_path=diff_path)
        self.assertTrue(changed["changed"])
        self.assertEqual(changed["method"], "file-hash")
        self.assertFalse(diff_path.exists())

    def test_resolve_submission_options_uses_branch_tip_when_branch_is_explicit(self):
        args = SimpleNamespace(
            branch="feature/topic",
            sha=None,
            targets=None,
            priority=None,
            smoke=False,
            allow_root_mismatch=True,
            allow_unreachable_targets=False,
        )

        original_load_config = self.mod.load_config
        original_resolve_targets = self.mod.resolve_targets
        original_default_priority = self.mod.default_priority_for
        original_resolve_ref = self.mod.resolve_git_ref_sha
        original_current_sha = self.mod.current_sha

        self.mod.load_config = lambda: {"targets": {"mac": {"type": "local", "enabled": True}}, "defaults": {}}
        self.mod.resolve_targets = lambda config, requested: ["mac"]
        self.mod.default_priority_for = lambda command, config: "normal"
        self.mod.resolve_git_ref_sha = lambda ref: "b" * 40
        self.mod.current_sha = lambda: "a" * 40
        try:
            _config, branch, sha, targets, priority, validation, submission = self.mod.resolve_submission_options(
                args, "run"
            )
        finally:
            self.mod.load_config = original_load_config
            self.mod.resolve_targets = original_resolve_targets
            self.mod.default_priority_for = original_default_priority
            self.mod.resolve_git_ref_sha = original_resolve_ref
            self.mod.current_sha = original_current_sha

        self.assertEqual(branch, "feature/topic")
        self.assertEqual(sha, "b" * 40)
        self.assertEqual(targets, ["mac"])
        self.assertEqual(priority, "normal")
        self.assertEqual(validation, "full")
        self.assertEqual(submission["branch"], "feature/topic")
        self.assertEqual(Path(submission["config_path"]).resolve(), self.config_path.resolve())

    def test_build_submission_metadata_rejects_root_mismatch_by_default(self):
        config = self.mod.load_config()
        original_root = self.mod.ROOT
        original_git_root = self.mod.git_root_for
        self.mod.ROOT = Path("/tmp/pulp-root")
        self.mod.git_root_for = lambda path: Path("/tmp/other-root")
        try:
            with self.assertRaises(ValueError):
                self.mod.build_submission_metadata(
                    config,
                    "feature/topic",
                    "a" * 40,
                    ["mac"],
                    "normal",
                    "full",
                    allow_root_mismatch=False,
                    allow_unreachable_targets=False,
                )
        finally:
            self.mod.ROOT = original_root
            self.mod.git_root_for = original_git_root

    def test_build_submission_metadata_records_fallback_host_preflight(self):
        config = {
            "targets": {
                "windows": {
                    "type": "ssh",
                    "enabled": True,
                    "host": "win2",
                    "fallback_host": "win",
                    "repo_path": "C:\\Pulp",
                }
            },
            "defaults": {},
        }
        original_ssh = self.mod.ssh_reachable
        self.mod.ssh_reachable = lambda host, timeout=5: host == "win"
        try:
            submission = self.mod.build_submission_metadata(
                config,
                "feature/topic",
                "a" * 40,
                ["windows"],
                "normal",
                "full",
                allow_root_mismatch=True,
                allow_unreachable_targets=False,
            )
        finally:
            self.mod.ssh_reachable = original_ssh

        state = submission["target_hosts"]["windows"]
        self.assertEqual(state["status"], "fallback-up")
        self.assertEqual(state["resolved_host"], "win")
        self.assertIn("fallback", submission["warnings"][0])

    def test_build_submission_metadata_fails_fast_for_unreachable_target_without_override(self):
        config = {
            "targets": {
                "windows": {
                    "type": "ssh",
                    "enabled": True,
                    "host": "win2",
                    "repo_path": "C:\\Pulp",
                }
            },
            "defaults": {},
        }
        original_ssh = self.mod.ssh_reachable
        self.mod.ssh_reachable = lambda host, timeout=5: False
        try:
            with self.assertRaises(ValueError):
                self.mod.build_submission_metadata(
                    config,
                    "feature/topic",
                    "a" * 40,
                    ["windows"],
                    "normal",
                    "full",
                    allow_root_mismatch=True,
                    allow_unreachable_targets=False,
                )
        finally:
            self.mod.ssh_reachable = original_ssh

    def test_parse_windows_ssh_json_rejects_non_object_payload(self):
        with self.assertRaises(RuntimeError):
            self.mod.parse_windows_ssh_json("null\n")

    def test_process_job_prefers_submission_config_path_for_windows_target(self):
        shared_config = {
            "targets": {
                "windows": {
                    "type": "ssh",
                    "enabled": True,
                    "host": "win",
                    "repo_path": r"C:\SharedPulp",
                    "cmake_generator": "Visual Studio 17 2022",
                }
            },
            "defaults": {},
        }
        submitted_config = {
            "targets": {
                "windows": {
                    "type": "ssh",
                    "enabled": True,
                    "host": "win2",
                    "repo_path": r"C:\WorktreePulp",
                    "cmake_generator": "Visual Studio 17 2022",
                }
            },
            "defaults": {},
        }
        submitted_path = Path(self.tmpdir.name) / "submitted-config.json"
        submitted_path.write_text(json.dumps(submitted_config) + "\n")
        job = self.mod.make_job(
            "feature/topic",
            "a" * 40,
            "normal",
            ["windows"],
            "run",
            "full",
            submission={
                "config_path": str(submitted_path),
                "target_hosts": {
                    "windows": {
                        "target": "windows",
                        "transport_mode": "bundle",
                        "configured_host": "win2",
                        "resolved_host": "win2",
                        "repo_path": r"C:\WorktreePulp",
                        "status": "primary-up",
                    }
                },
            },
        )

        captured = {}
        original_run_windows = self.mod.run_windows_ssh_validation
        self.mod.run_windows_ssh_validation = lambda target_name, host, repo_path, queued_job, **kwargs: {
            "target": target_name,
            "status": "pass",
            "exit_code": 0,
            "duration_secs": 0.0,
            "stdout_tail": "",
            "stderr_tail": "",
            "log_file": "",
            "host": captured.setdefault("host", host),
            "repo_path": captured.setdefault("repo_path", repo_path),
        }
        try:
            result = self.mod.process_job(job, shared_config)
        finally:
            self.mod.run_windows_ssh_validation = original_run_windows

        self.assertEqual(result["overall"], "pass")
        self.assertEqual(captured["host"], "win2")
        self.assertEqual(captured["repo_path"], r"C:\WorktreePulp")

    def test_build_target_tasks_binds_repo_paths_per_target(self):
        config = {
            "targets": {
                "ubuntu": {
                    "type": "ssh",
                    "enabled": True,
                    "host": "ubuntu",
                    "repo_path": "/home/daniel/Code/pulp-validate",
                },
                "windows": {
                    "type": "ssh",
                    "enabled": True,
                    "host": "win",
                    "repo_path": r"C:\Users\danielraffel\pulp-validate",
                    "cmake_generator": "Visual Studio 17 2022",
                },
            },
            "defaults": {},
        }
        job = self.mod.make_job(
            "feature/topic",
            "a" * 40,
            "normal",
            ["ubuntu", "windows"],
            "run",
            "full",
        )
        captured = {}
        original_host = self.mod.ensure_host_reachable
        original_posix = self.mod.run_posix_ssh_validation
        original_windows = self.mod.run_windows_ssh_validation
        self.mod.ensure_host_reachable = lambda target_name, target_cfg, defaults: target_cfg["host"]
        self.mod.run_posix_ssh_validation = lambda target_name, host, repo_path, queued_job, **kwargs: {
            "target": target_name,
            "status": "pass",
            "exit_code": 0,
            "duration_secs": 0.0,
            "stdout_tail": "",
            "stderr_tail": "",
            "log_file": "",
            "repo_path": captured.setdefault("ubuntu_repo_path", repo_path),
        }
        self.mod.run_windows_ssh_validation = lambda target_name, host, repo_path, queued_job, **kwargs: {
            "target": target_name,
            "status": "pass",
            "exit_code": 0,
            "duration_secs": 0.0,
            "stdout_tail": "",
            "stderr_tail": "",
            "log_file": "",
            "repo_path": captured.setdefault("windows_repo_path", repo_path),
        }
        try:
            tasks = self.mod._build_target_tasks(job, config)
            for _name, fn in tasks:
                fn()
        finally:
            self.mod.ensure_host_reachable = original_host
            self.mod.run_posix_ssh_validation = original_posix
            self.mod.run_windows_ssh_validation = original_windows

        self.assertEqual(captured["ubuntu_repo_path"], "/home/daniel/Code/pulp-validate")
        self.assertEqual(captured["windows_repo_path"], r"C:\Users\danielraffel\pulp-validate")

    def test_load_config_normalizes_desktop_automation_defaults(self):
        config = self.mod.load_config()

        desktop = config["desktop_automation"]
        self.assertEqual(desktop["publish_mode"], "none")
        self.assertEqual(desktop["publish_branch"], "dev-artifacts")
        self.assertEqual(desktop["retention_days"], 14)
        self.assertTrue(desktop["artifact_root"])
        self.assertEqual(desktop["targets"]["mac"]["adapter"], "macos-local")
        self.assertEqual(desktop["targets"]["mac"]["capability_tier"], "v2")
        self.assertEqual(desktop["targets"]["ubuntu"]["adapter"], "linux-xvfb")
        self.assertEqual(desktop["targets"]["windows"]["adapter"], "windows-session-agent")
        self.assertEqual(desktop["targets"]["windows"]["capability_tier"], "v2")
        self.assertEqual(desktop["targets"]["windows"]["task_name"], None)
        self.assertEqual(desktop["targets"]["windows"]["remote_root"], None)
        contract = self.mod.desktop_target_contract("windows", desktop["targets"]["windows"])
        self.assertEqual(contract["task_name"], "PulpDesktopAutomationAgent-windows")
        self.assertTrue(contract["remote_root"].startswith("%LOCALAPPDATA%"))

    def test_build_windows_session_agent_request_sets_outputs_and_env(self):
        config = self.mod.load_config()
        contract = self.mod.desktop_target_contract("windows", config["desktop_automation"]["targets"]["windows"])
        request = self.mod.build_windows_session_agent_request(
            "windows",
            contract,
            r"C:\Pulp\build\ui-preview.exe",
            repo_path=r"C:\Pulp",
            action_name="click",
            label="ui-preview",
            pulp_app_automation=True,
            capture_ui_snapshot=True,
            click_point=None,
            click_view_id="bypass-toggle",
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            capture_before=True,
            settle_secs=0.75,
            timeout_secs=20.0,
        )

        self.assertEqual(request["schema"], 1)
        self.assertEqual(request["target"], "windows")
        self.assertEqual(request["action"], "click")
        self.assertEqual(request["cwd"], r"C:\Pulp")
        self.assertEqual(request["execution"]["capture_mode"], "pulp-app")
        self.assertTrue(request["execution"]["capture_ui_snapshot"])
        self.assertTrue(request["execution"]["capture_before"])
        self.assertEqual(request["interaction"]["view_id"], "bypass-toggle")
        self.assertEqual(request["env"]["PULP_AUTOMATION_CLICK_VIEW_ID"], "bypass-toggle")
        self.assertEqual(request["env"]["PULP_AUTOMATION_AFTER_DELAY_MS"], "750")
        self.assertIn("\\results\\", request["outputs"]["screenshot"])
        self.assertIn("ui-tree.json", request["outputs"]["ui_snapshot"])
        self.assertIn("before.png", request["outputs"]["before_screenshot"])

    def test_make_desktop_source_request_defaults_to_live_current_branch_and_sha(self):
        args = SimpleNamespace(
            source_mode=None,
            branch=None,
            sha=None,
            prepare_command=None,
            prepare_timeout=None,
        )
        with mock.patch.object(self.mod, "current_branch", return_value="feature/test"):
            with mock.patch.object(self.mod, "current_sha", return_value="a" * 40):
                request = self.mod.make_desktop_source_request(args)

        self.assertEqual(request["mode"], "live")
        self.assertEqual(request["branch"], "feature/test")
        self.assertEqual(request["sha"], "a" * 40)
        self.assertEqual(request["prepare_timeout_secs"], 900.0)

    def test_rewrite_launch_command_helpers_retarget_repo_local_paths(self):
        command = f"{self.mod.ROOT}/build/ui-preview --flag"
        source_root = Path(self.tmpdir.name) / "prepared"

        local = self.mod.rewrite_launch_command_for_source_root(command, source_root)
        linux_remote = self.mod.rewrite_launch_command_for_posix_root(command, "$HOME/.local/state/pulp/source")
        windows_remote = self.mod.rewrite_launch_command_for_windows_root(command, r"C:\Users\daniel\AppData\Local\Pulp\source")

        self.assertIn(str(source_root / "build" / "ui-preview"), local)
        self.assertIn("$HOME/.local/state/pulp/source/build/ui-preview", linux_remote)
        self.assertIn(r"C:\Users\daniel\AppData\Local\Pulp\source\build\ui-preview", windows_remote)

    def test_rewrite_launch_command_for_windows_root_uses_windows_quoting(self):
        command = r'.\build-desktop-automation\examples\ui-preview\Debug\pulp-ui-preview.exe --flag'

        rewritten = self.mod.rewrite_launch_command_for_windows_root(command, r'C:\Program Files\Pulp\desktop-source\windows\abc123')

        self.assertIn(r'"C:\Program Files\Pulp\desktop-source\windows\abc123\build-desktop-automation\examples\ui-preview\Debug\pulp-ui-preview.exe" --flag', rewritten)
        self.assertNotIn("'", rewritten)

    def test_rewrite_launch_command_helpers_support_windows_relative_tokens(self):
        command = r".\build-desktop-automation\examples\ui-preview\Debug\pulp-ui-preview.exe --flag"

        rewritten = self.mod.rewrite_launch_command_for_windows_root(command, r"%LOCALAPPDATA%\Pulp\desktop-source\windows\abc123")

        self.assertIn(r"%LOCALAPPDATA%\Pulp\desktop-source\windows\abc123\build-desktop-automation\examples\ui-preview\Debug\pulp-ui-preview.exe", rewritten)

    def test_split_windows_prepare_commands_preserves_quoted_generator(self):
        commands = self.mod.split_windows_prepare_commands(
            'cmake -S . -B build -G "Visual Studio 17 2022" -A x64; cmake --build build --config Debug'
        )

        self.assertEqual(
            commands,
            [
                'cmake -S . -B build -G "Visual Studio 17 2022" -A x64',
                "cmake --build build --config Debug",
            ],
        )

    def test_validate_windows_prepare_commands_rejects_single_quoted_tokens(self):
        with self.assertRaises(ValueError) as ctx:
            self.mod.validate_windows_prepare_commands(
                ["cmake -S . -B build -G 'Visual Studio 17 2022' -A x64"]
            )

        self.assertIn("single-quoted tokens are literal text", str(ctx.exception))

    def test_validate_windows_prepare_commands_accepts_double_quoted_tokens(self):
        self.mod.validate_windows_prepare_commands(
            ['cmake -S . -B build -G "Visual Studio 17 2022" -A x64']
        )

    def test_run_ssh_subprocess_retries_transient_connection_reset(self):
        calls = []

        def fake_run(*args, **kwargs):
            calls.append((args, kwargs))
            if len(calls) == 1:
                return subprocess.CompletedProcess(args[0][0], 255, "", "kex_exchange_identification: read: Connection reset by peer")
            return subprocess.CompletedProcess(args[0][0], 0, "ok\n", "")

        with mock.patch.object(self.mod.subprocess, "run", side_effect=fake_run):
            with mock.patch.object(self.mod.time, "sleep") as sleep_mock:
                result = self.mod.run_ssh_subprocess(["ssh", "win2", "hostname"], timeout=5)

        self.assertEqual(result.returncode, 0)
        self.assertEqual(len(calls), 2)
        sleep_mock.assert_called_once()

    def test_run_ssh_subprocess_does_not_retry_non_transient_failure(self):
        calls = []

        def fake_run(*args, **kwargs):
            calls.append((args, kwargs))
            return subprocess.CompletedProcess(args[0][0], 1, "", "permission denied")

        with mock.patch.object(self.mod.subprocess, "run", side_effect=fake_run):
            with mock.patch.object(self.mod.time, "sleep") as sleep_mock:
                result = self.mod.run_ssh_subprocess(["ssh", "win2", "hostname"], timeout=5)

        self.assertEqual(result.returncode, 1)
        self.assertEqual(len(calls), 1)
        sleep_mock.assert_not_called()

    def test_prepare_linux_exact_sha_source_fetches_bundle_ref_without_lfs_smudge(self):
        bundle_dir = Path(self.tmpdir.name) / "bundle-linux"
        bundle_dir.mkdir(parents=True, exist_ok=True)
        source_request = {
            "mode": "exact-sha",
            "branch": "feature/source",
            "sha": "e" * 40,
            "prepare_timeout_secs": 120.0,
        }
        captured = {}

        def fake_run(cmd, **kwargs):
            captured["cmd"] = cmd
            return SimpleNamespace(returncode=0, stdout="__PULP_PREPARED__:clean\n", stderr="")

        with mock.patch.object(self.mod, "sync_job_bundle_to_ssh_host", return_value=("bundle.git", "refs/pulp-ci-bundles/test")):
            with mock.patch.object(self.mod.subprocess, "run", side_effect=fake_run):
                with mock.patch.object(self.mod, "fetch_ssh_artifact"):
                    context = self.mod.prepare_linux_exact_sha_source(
                        bundle_dir,
                        "ubuntu",
                        "ubuntu",
                        "./build/ui-preview --flag",
                        source_request,
                    )

        remote_script = captured["cmd"][-1]
        self.assertIn('export PATH="$HOME/.local/bin:$PATH"', remote_script)
        self.assertIn("export GIT_LFS_SKIP_SMUDGE=1", remote_script)
        self.assertIn("bundle_ref=refs/pulp-ci-bundles/test", remote_script)
        self.assertIn('git -C "$prepared_root" init --quiet', remote_script)
        self.assertIn('git -C "$prepared_root" fetch "$bundle" "$bundle_ref:refs/pulp-ci-bundles/source" >/dev/null 2>&1', remote_script)
        self.assertIn('git -C "$prepared_root" remote add origin "$remote_url"', remote_script)
        self.assertEqual(context["prepared_state"], "clean")

    def test_prepare_linux_exact_sha_source_requires_prepare_stamp_when_prepare_command_exists(self):
        bundle_dir = Path(self.tmpdir.name) / "bundle-linux-prepare"
        bundle_dir.mkdir(parents=True, exist_ok=True)
        source_request = {
            "mode": "exact-sha",
            "branch": "feature/source",
            "sha": "f" * 40,
            "prepare_command": "./scripts/build-ui-preview.sh",
            "prepare_timeout_secs": 120.0,
        }
        captured = {}

        def fake_run(cmd, **kwargs):
            captured["cmd"] = cmd
            return SimpleNamespace(returncode=0, stdout="__PULP_PREPARED__:clean\n", stderr="")

        with mock.patch.object(self.mod, "sync_job_bundle_to_ssh_host", return_value=("bundle.git", "refs/pulp-ci-bundles/test")):
            with mock.patch.object(self.mod.subprocess, "run", side_effect=fake_run):
                with mock.patch.object(self.mod, "fetch_ssh_artifact"):
                    context = self.mod.prepare_linux_exact_sha_source(
                        bundle_dir,
                        "ubuntu",
                        "ubuntu",
                        "./build/ui-preview --flag",
                        source_request,
                    )

        remote_script = captured["cmd"][-1]
        self.assertIn("export PULP_REQUIRE_PREPARE_STAMP=1", remote_script)
        self.assertIn('if [ ! -f "$prepare_stamp" ] && [ -n "${PULP_REQUIRE_PREPARE_STAMP:-}" ]; then reused=0; else reused=1; fi', remote_script)
        self.assertIn("printf", remote_script)
        self.assertIn("> \"$prepare_stamp\"", remote_script)
        self.assertEqual(context["prepared_state"], "clean")

    def test_prepare_windows_exact_sha_source_expands_environment_aware_paths(self):
        bundle_dir = Path(self.tmpdir.name) / "bundle"
        bundle_dir.mkdir(parents=True, exist_ok=True)
        source_request = {
            "mode": "exact-sha",
            "branch": "feature/source",
            "sha": "d" * 40,
            "prepare_command": ".\\scripts\\build-ui-preview.ps1",
            "prepare_timeout_secs": 120.0,
        }
        scripts = []

        def fake_run_windows_ssh_powershell(host, ps_script, *, timeout=60):
            scripts.append(ps_script)
            return SimpleNamespace(returncode=0, stdout="__PULP_PREPARED__:clean\n", stderr="")

        with mock.patch.object(self.mod, "sync_job_bundle_to_ssh_host", return_value=("bundle.git", "refs/pulp-ci-bundles/test")):
            with mock.patch.object(self.mod, "run_windows_ssh_powershell", side_effect=fake_run_windows_ssh_powershell):
                with mock.patch.object(self.mod, "windows_ssh_fetch_file"):
                    context = self.mod.prepare_windows_exact_sha_source(
                        bundle_dir,
                        "windows",
                        "win",
                        r".\build-desktop-automation\examples\ui-preview\Debug\pulp-ui-preview.exe",
                        source_request,
                    )

        script = scripts[0]
        self.assertIn("$env:GIT_LFS_SKIP_SMUDGE = '1'", script)
        self.assertIn("$Bundle = Join-Path $HOME 'bundle.git'", script)
        self.assertIn("$BundleRef = 'refs/pulp-ci-bundles/test'", script)
        self.assertIn("$PreparedRoot = [Environment]::ExpandEnvironmentVariables(", script)
        self.assertIn("$RemotePrepareLog = [Environment]::ExpandEnvironmentVariables(", script)
        self.assertIn("$PrepareStamp = [Environment]::ExpandEnvironmentVariables(", script)
        self.assertIn("$env:PULP_REQUIRE_PREPARE_STAMP = '1'", script)
        self.assertIn("$Sha = 'dddddddddddddddddddddddddddddddddddddddd'", script)
        self.assertIn("$PreparedHead = $null", script)
        self.assertIn("$PreparedHead = git -C $PreparedRoot rev-parse HEAD 2>$null", script)
        self.assertIn("if ($Reused -and $env:PULP_REQUIRE_PREPARE_STAMP -and -not (Test-Path $PrepareStamp)) { $Reused = $false }", script)
        self.assertIn('cmd.exe /c "rmdir /s /q \\"$PreparedRoot\\""', script)
        self.assertIn("git -C $PreparedRoot init --quiet | Out-Null", script)
        self.assertIn("git -C $PreparedRoot fetch $Bundle \"$BundleRef`:refs/pulp-ci-bundles/source\" | Out-Null", script)
        self.assertIn("Set-Content -LiteralPath $PrepareStamp -Value $Sha -Encoding UTF8", script)
        self.assertIn("$PrepareScriptPath = [Environment]::ExpandEnvironmentVariables(", script)
        self.assertIn("@'", script)
        self.assertIn("@echo off", script)
        self.assertIn("cd /d \"%~dp0\"", script)
        self.assertIn(".\\scripts\\build-ui-preview.ps1", script)
        self.assertIn("if errorlevel 1 exit /b %errorlevel%", script)
        self.assertIn("Set-Content -LiteralPath $PrepareScriptPath -Encoding UTF8", script)
        self.assertIn("$PrepareCmd = ('\"{0}\" > \"{1}\" 2>&1' -f $PrepareScriptPath, $RemotePrepareLog)", script)
        self.assertIn("cmd.exe /c $PrepareCmd | Out-Null", script)
        self.assertIn("Remove-Item -LiteralPath $PrepareScriptPath -Force", script)
        self.assertEqual(context["prepared_state"], "clean")
        self.assertIn(r"%LOCALAPPDATA%\Pulp\desktop-source\windows", context["launch_command"])

    def test_desktop_parser_accepts_shared_exact_sha_source_flags(self):
        parser = self.mod.build_parser()

        smoke = parser.parse_args(
            [
                "desktop",
                "smoke",
                "mac",
                "--command",
                "/tmp/ui-preview",
                "--source-mode",
                "exact-sha",
                "--branch",
                "feature/ui",
                "--sha",
                "a" * 40,
                "--prepare-command",
                "./scripts/build-ui-preview.sh",
                "--prepare-timeout",
                "321",
            ]
        )
        click = parser.parse_args(
            [
                "desktop",
                "click",
                "ubuntu",
                "--command",
                "/tmp/ui-preview",
                "--click-view-id",
                "bypass-toggle",
                "--source-mode",
                "exact-sha",
            ]
        )
        inspect = parser.parse_args(
            [
                "desktop",
                "inspect",
                "windows",
                "--command",
                r"C:\Pulp\ui-preview.exe",
            ]
        )

        self.assertEqual(smoke.source_mode, "exact-sha")
        self.assertEqual(smoke.branch, "feature/ui")
        self.assertEqual(smoke.sha, "a" * 40)
        self.assertEqual(smoke.prepare_command, "./scripts/build-ui-preview.sh")
        self.assertEqual(smoke.prepare_timeout, 321.0)
        self.assertEqual(click.source_mode, "exact-sha")
        self.assertEqual(inspect.source_mode, "live")

    def test_desktop_parser_accepts_proof_filters(self):
        parser = self.mod.build_parser()

        proof = parser.parse_args(
            [
                "desktop",
                "proof",
                "windows",
                "--action",
                "inspect",
                "--source-mode",
                "legacy",
                "--sha",
                "a" * 40,
                "--branch",
                "feature/ui",
                "--limit",
                "7",
            ]
        )

        self.assertEqual(proof.desktop_command, "proof")
        self.assertEqual(proof.target, "windows")
        self.assertEqual(proof.action, "inspect")
        self.assertEqual(proof.source_mode, "legacy")
        self.assertEqual(proof.sha, "a" * 40)
        self.assertEqual(proof.branch, "feature/ui")
        self.assertEqual(proof.limit, 7)

    def test_build_linux_xvfb_remote_command_uses_launch_cwd(self):
        remote_cmd = self.mod.build_linux_xvfb_remote_command(
            "/tmp/pulp",
            ".local/state/pulp/desktop-automation/remote/ubuntu/smoke/run-1",
            "./build/ui-preview",
            launch_cwd="$HOME/.local/state/pulp/desktop-source/ubuntu/abc123",
            capture_ui_snapshot=False,
            click_point=None,
            click_view_id=None,
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            capture_before=False,
            settle_secs=0.5,
        )

        self.assertIn('launch_cwd=$HOME/.local/state/pulp/desktop-source/ubuntu/abc123', remote_cmd)
        self.assertIn('cd "$launch_cwd"', remote_cmd)

    def test_build_linux_xvfb_remote_command_supports_existing_display(self):
        remote_cmd = self.mod.build_linux_xvfb_remote_command(
            "/tmp/pulp",
            ".local/state/pulp/desktop-automation/remote/ubuntu/smoke/run-1",
            "./build/ui-preview",
            launch_backend={"mode": "display", "display": ":0", "xdg_runtime_dir": "/run/user/1000"},
            capture_ui_snapshot=False,
            click_point=None,
            click_view_id=None,
            click_view_type=None,
            click_view_text=None,
            click_view_label=None,
            capture_before=False,
            settle_secs=0.0,
        )

        self.assertIn("export DISPLAY=:0", remote_cmd)
        self.assertIn("export XDG_RUNTIME_DIR=/run/user/1000", remote_cmd)
        self.assertNotIn("xvfb-run -a", remote_cmd)
        self.assertIn("bash -lc ./build/ui-preview", remote_cmd)

    def test_ssh_failure_detail_reports_handshake_reset(self):
        result = subprocess.CompletedProcess(["ssh"], 255, "", "kex_exchange_identification: read: Connection reset by peer\nConnection reset by 100.110.129.45 port 22\n")
        with mock.patch.object(self.mod, "ssh_probe", return_value=result):
            detail = self.mod.ssh_failure_detail("win2", 5)

        self.assertEqual(detail, "win2 (SSH service reset during handshake; verify OpenSSH server on the target)")

    def test_ssh_probe_timeout_returns_completed_process(self):
        with mock.patch.object(self.mod, "run_ssh_subprocess", side_effect=subprocess.TimeoutExpired(["ssh"], 5)):
            result = self.mod.ssh_probe("win2", 5)

        self.assertEqual(result.returncode, 124)
        self.assertIn("timed out", result.stderr.lower())

    def test_cmd_desktop_install_records_receipt(self):
        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_desktop_install(SimpleNamespace(target="ubuntu"))

        self.assertEqual(exit_code, 0)
        receipt = self.mod.desktop_receipt_for("ubuntu")
        self.assertIsNotNone(receipt)
        self.assertEqual(receipt["target"], "ubuntu")
        self.assertEqual(receipt["adapter"], "linux-xvfb")
        self.assertFalse(receipt["remote_bootstrap_ready"])
        self.assertTrue(Path(receipt["artifact_root"]).exists())
        self.assertIn("Desktop target `ubuntu` prepared.", buf.getvalue())

    def test_bootstrap_windows_session_agent_registers_real_script_arguments(self):
        self._set_target_enabled("windows", True)
        contract = self.mod.desktop_target_contract("windows", self.mod.load_config()["desktop_automation"]["targets"]["windows"])
        scripts = []

        def fake_run_windows_ssh_powershell(host, ps_script, *, timeout=60):
            scripts.append(ps_script)
            return SimpleNamespace(returncode=0, stdout='{"task_name":"PulpDesktopAutomationAgent-windows","task_present":true,"task_state":"Ready"}', stderr='')

        with mock.patch.object(self.mod, "windows_ssh_write_text"):
            with mock.patch.object(self.mod, "run_windows_ssh_powershell", side_effect=fake_run_windows_ssh_powershell):
                result = self.mod.bootstrap_windows_session_agent("win", contract)

        script = scripts[0]
        self.assertIn('-File "{0}" -RemoteRoot "{1}"', script)
        self.assertEqual(result["task_name"], "PulpDesktopAutomationAgent-windows")

    def test_cmd_desktop_install_records_windows_contract(self):
        self._set_target_enabled("windows", True)
        with mock.patch.object(self.mod, "ensure_host_reachable", return_value="win2"):
            with mock.patch.object(
                self.mod,
                "bootstrap_windows_session_agent",
                return_value={
                    "task_name": "PulpDesktopAutomationAgent-windows",
                    "remote_root": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent",
                    "script_path": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\agent.ps1",
                    "script_exists": True,
                    "jobs_dir_exists": True,
                    "results_dir_exists": True,
                },
            ):
                with mock.patch.object(
                    self.mod,
                    "probe_windows_session_agent",
                    return_value={
                        "task_name": "PulpDesktopAutomationAgent-windows",
                        "task_present": True,
                        "task_state": "Ready",
                        "interactive_user": r"DESKTOP\daniel",
                        "remote_root": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent",
                        "agent_root_exists": True,
                        "jobs_dir": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\jobs",
                        "jobs_dir_exists": True,
                        "results_dir": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\results",
                        "results_dir_exists": True,
                        "script_path": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\agent.ps1",
                        "script_exists": True,
                    },
                ):
                    with mock.patch.object(
                        self.mod,
                        "ensure_windows_remote_tooling",
                        return_value={
                            "probe": {
                                "git_found": True,
                                "git_path": r"C:\Program Files\Git\cmd\git.exe",
                                "git_version": "git version 2.49.0.windows.1",
                                "gh_found": False,
                                "winget_found": True,
                            },
                            "installed": ["git"],
                        },
                    ):
                        with mock.patch.object(
                            self.mod,
                            "ensure_windows_remote_repo_checkout",
                            return_value={
                                "home_dir": r"C:\Users\daniel",
                                "repo_path": r"C:\Users\daniel\pulp-validate",
                                "repo_exists": True,
                                "git_dir_exists": True,
                                "head_exists": True,
                                "setup_exists": True,
                                "origin_url": "https://github.com/danielraffel/pulp",
                                "repo_path_unsafe": False,
                            },
                        ):
                            buf = io.StringIO()
                            with redirect_stdout(buf):
                                with mock.patch.object(
                                    self.mod.subprocess,
                                    "run",
                                    return_value=subprocess.CompletedProcess(["git"], 0, "a" * 40 + "\n", ""),
                                ):
                                    with mock.patch.object(
                                        self.mod,
                                        "sync_job_bundle_to_ssh_host",
                                        return_value=("bundle.git", "refs/pulp-ci-bundles/install"),
                                    ):
                                        exit_code = self.mod.cmd_desktop_install(SimpleNamespace(target="windows"))

        self.assertEqual(exit_code, 0)
        receipt = self.mod.desktop_receipt_for("windows")
        self.assertIsNotNone(receipt)
        self.assertEqual(receipt["adapter"], "windows-session-agent")
        self.assertTrue(receipt["remote_bootstrap_ready"])
        self.assertTrue(receipt["remote_tooling_ready"])
        self.assertTrue(receipt["remote_repo_checkout_ready"])
        self.assertEqual(receipt["contract"]["task_name"], "PulpDesktopAutomationAgent-windows")
        self.assertIn("remote tooling: ready", buf.getvalue())
        self.assertIn("remote tooling installed: git", buf.getvalue())
        self.assertIn("task_name: PulpDesktopAutomationAgent-windows", buf.getvalue())
        self.assertIn(r"remote repo checkout: C:\Users\daniel\pulp-validate", buf.getvalue())

    def test_cmd_desktop_doctor_reports_remote_target_health(self):
        self.mod.cmd_desktop_install(SimpleNamespace(target="ubuntu"))

        with mock.patch.object(self.mod, "ssh_reachable", return_value=True):
            with mock.patch.object(
                self.mod,
                "probe_linux_launch_backend",
                return_value={"mode": "xvfb", "path": "/usr/bin/xvfb-run"},
            ):
                with mock.patch.object(
                    self.mod,
                    "probe_linux_remote_tooling",
                    return_value={
                        "git_found": True,
                        "git_version": "git version 2.49.0",
                        "git_path": "/usr/bin/git",
                        "git_lfs_found": True,
                        "git_lfs_version": "git-lfs/3.5.1",
                        "git_lfs_path": "/usr/bin/git-lfs",
                        "xvfb_run_found": True,
                        "xvfb_run_version": "Usage: xvfb-run [OPTION ...] COMMAND",
                        "xvfb_run_path": "/usr/bin/xvfb-run",
                        "xauth_found": True,
                        "xauth_version": "1.1.2",
                        "xauth_path": "/usr/bin/xauth",
                        "xdotool_found": True,
                        "xdotool_version": "xdotool version 3.20211022.1",
                        "xdotool_path": "/usr/bin/xdotool",
                        "xwininfo_found": True,
                        "xwininfo_version": "xwininfo 1.3.4",
                        "xwininfo_path": "/usr/bin/xwininfo",
                        "import_found": True,
                        "import_version": "Version: ImageMagick 6.9.12-98",
                        "import_path": "/usr/bin/import",
                        "wmctrl_found": True,
                        "wmctrl_version": "1.07",
                        "wmctrl_path": "/usr/bin/wmctrl",
                    },
                ):
                    buf = io.StringIO()
                    with redirect_stdout(buf):
                        exit_code = self.mod.cmd_desktop_doctor(SimpleNamespace(target="ubuntu"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop doctor for `ubuntu`", output)
        self.assertIn("PASS  receipt: installed", output)
        self.assertIn("PASS  ssh: ubuntu", output)
        self.assertIn("PASS  launch_backend: /usr/bin/xvfb-run", output)
        self.assertIn("PASS  git-lfs: git-lfs/3.5.1 (/usr/bin/git-lfs)", output)
        self.assertIn("PASS  xdotool: xdotool version 3.20211022.1 (/usr/bin/xdotool)", output)

    def test_cmd_desktop_doctor_reports_linux_xvfb_remediation(self):
        self.mod.cmd_desktop_install(SimpleNamespace(target="ubuntu"))

        with mock.patch.object(self.mod, "ssh_reachable", return_value=True):
            with mock.patch.object(
                self.mod,
                "probe_linux_launch_backend",
                return_value={"mode": "missing"},
            ):
                with mock.patch.object(
                    self.mod,
                    "probe_linux_remote_tooling",
                    return_value={
                        "git_found": True,
                        "git_version": "git version 2.49.0",
                        "git_path": "/usr/bin/git",
                        "git_lfs_found": False,
                        "git_lfs_path": "/home/daniel/.local/bin/git-lfs",
                        "git_lfs_version": "git-lfs/3.7.0",
                        "git_lfs_hint": "installed at /home/daniel/.local/bin/git-lfs but unavailable to non-interactive shells; add $HOME/.local/bin to PATH or install git-lfs system-wide",
                        "xvfb_run_found": False,
                        "xauth_found": False,
                        "xdotool_found": False,
                        "xwininfo_found": True,
                        "xwininfo_version": "xwininfo 1.3.4",
                        "xwininfo_path": "/usr/bin/xwininfo",
                        "import_found": False,
                        "wmctrl_found": False,
                    },
                ):
                    buf = io.StringIO()
                    with redirect_stdout(buf):
                        exit_code = self.mod.cmd_desktop_doctor(SimpleNamespace(target="ubuntu"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 1)
        self.assertIn("install xvfb and xauth", output)
        self.assertIn("FAIL  git-lfs: installed at /home/daniel/.local/bin/git-lfs but unavailable to non-interactive shells", output)
        self.assertIn("FAIL  xdotool: missing; sudo apt-get install -y xdotool", output)
        self.assertIn("FAIL  import: missing; sudo apt-get install -y imagemagick", output)
        self.assertIn("WARN  wmctrl: missing; sudo apt-get install -y wmctrl", output)

    def test_cmd_desktop_doctor_reports_windows_ssh_handshake_reset(self):
        self._set_target_enabled("windows", True)
        with mock.patch.object(self.mod, "ensure_host_reachable", return_value="win2"):
            with mock.patch.object(
                self.mod,
                "ensure_windows_remote_repo_checkout",
                return_value={
                    "home_dir": r"C:\Users\danielraffel",
                    "repo_path": r"C:\Users\danielraffel\pulp-validate",
                    "repo_exists": True,
                    "git_dir_exists": True,
                    "origin_url": "https://github.com/danielraffel/pulp",
                    "repo_path_unsafe": False,
                },
            ):
                with mock.patch.object(
                    self.mod.subprocess,
                    "run",
                    return_value=subprocess.CompletedProcess(["git"], 0, "a" * 40 + "\n", ""),
                ):
                    with mock.patch.object(
                        self.mod,
                        "sync_job_bundle_to_ssh_host",
                        return_value=("bundle.git", "refs/pulp-ci-bundles/install"),
                    ):
                        self.mod.cmd_desktop_install(SimpleNamespace(target="windows"))

        with mock.patch.object(self.mod, "ssh_reachable", return_value=False):
            with mock.patch.object(
                self.mod,
                "ssh_failure_detail",
                return_value="win2 (SSH service reset during handshake; verify OpenSSH server on the target)",
            ):
                buf = io.StringIO()
                with redirect_stdout(buf):
                    exit_code = self.mod.cmd_desktop_doctor(SimpleNamespace(target="windows"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 1)
        self.assertIn("FAIL  ssh: win2 (SSH service reset during handshake", output)

    def test_cmd_desktop_doctor_accepts_existing_linux_display_session(self):
        self.mod.cmd_desktop_install(SimpleNamespace(target="ubuntu"))

        with mock.patch.object(self.mod, "ssh_reachable", return_value=True):
            with mock.patch.object(
                self.mod,
                "probe_linux_launch_backend",
                return_value={"mode": "display", "display": ":0", "xdg_runtime_dir": "/run/user/1000"},
            ):
                with mock.patch.object(
                    self.mod,
                    "probe_linux_remote_tooling",
                    return_value={
                        "git_found": True,
                        "git_version": "git version 2.49.0",
                        "git_path": "/usr/bin/git",
                        "git_lfs_found": True,
                        "git_lfs_version": "git-lfs/3.5.1",
                        "git_lfs_path": "/usr/bin/git-lfs",
                        "xvfb_run_found": True,
                        "xvfb_run_version": "Usage: xvfb-run [OPTION ...] COMMAND",
                        "xvfb_run_path": "/usr/bin/xvfb-run",
                        "xauth_found": True,
                        "xauth_version": "1.1.2",
                        "xauth_path": "/usr/bin/xauth",
                        "xdotool_found": True,
                        "xdotool_version": "xdotool version 3.20211022.1",
                        "xdotool_path": "/usr/bin/xdotool",
                        "xwininfo_found": True,
                        "xwininfo_version": "xwininfo 1.3.4",
                        "xwininfo_path": "/usr/bin/xwininfo",
                        "import_found": True,
                        "import_version": "Version: ImageMagick 6.9.12-98",
                        "import_path": "/usr/bin/import",
                        "wmctrl_found": True,
                        "wmctrl_version": "1.07",
                        "wmctrl_path": "/usr/bin/wmctrl",
                    },
                ):
                    buf = io.StringIO()
                    with redirect_stdout(buf):
                        exit_code = self.mod.cmd_desktop_doctor(SimpleNamespace(target="ubuntu"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("PASS  launch_backend: existing display :0", output)


    def test_cmd_desktop_doctor_reports_windows_session_contract(self):
        self._set_target_enabled("windows", True)
        with mock.patch.object(self.mod, "ensure_host_reachable", return_value="win2"):
            with mock.patch.object(
                self.mod,
                "bootstrap_windows_session_agent",
                return_value={
                    "task_name": "PulpDesktopAutomationAgent-windows",
                    "remote_root": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent",
                    "script_path": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\agent.ps1",
                    "script_exists": True,
                    "jobs_dir_exists": True,
                    "results_dir_exists": True,
                },
            ):
                with mock.patch.object(
                    self.mod,
                    "probe_windows_session_agent",
                    return_value={
                        "task_name": "PulpDesktopAutomationAgent-windows",
                        "task_present": False,
                        "task_state": "",
                        "interactive_user": r"DESKTOP\daniel",
                        "remote_root": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent",
                        "agent_root_exists": False,
                        "jobs_dir": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\jobs",
                        "jobs_dir_exists": False,
                        "results_dir": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\results",
                        "results_dir_exists": False,
                        "script_path": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\agent.ps1",
                        "script_exists": False,
                    },
                ):
                    with mock.patch.object(
                        self.mod,
                        "ensure_windows_remote_tooling",
                        return_value={
                            "probe": {
                                "git_found": True,
                                "git_path": r"C:\Program Files\Git\cmd\git.exe",
                                "git_version": "git version 2.49.0.windows.1",
                                "gh_found": False,
                                "winget_found": True,
                                "gh_auth_ready": None,
                            },
                            "installed": [],
                        },
                    ):
                        with mock.patch.object(
                            self.mod,
                            "ensure_windows_remote_repo_checkout",
                            return_value={
                                "home_dir": r"C:\Users\danielraffel",
                                "repo_path": r"C:\Users\danielraffel\pulp-validate",
                                "repo_exists": True,
                                "git_dir_exists": True,
                                "head_exists": True,
                                "setup_exists": True,
                                "origin_url": "https://github.com/danielraffel/pulp",
                                "repo_path_unsafe": False,
                            },
                        ):
                            with mock.patch.object(self.mod, "ssh_reachable", return_value=True):
                                with mock.patch.object(
                                    self.mod.subprocess,
                                    "run",
                                    return_value=subprocess.CompletedProcess(["git"], 0, "a" * 40 + "\n", ""),
                                ):
                                    with mock.patch.object(
                                        self.mod,
                                        "sync_job_bundle_to_ssh_host",
                                        return_value=("bundle.git", "refs/pulp-ci-bundles/install"),
                                    ):
                                        self.mod.cmd_desktop_install(SimpleNamespace(target="windows"))
                    with mock.patch.object(
                        self.mod,
                        "probe_windows_remote_tooling",
                        return_value={
                            "git_found": True,
                            "git_path": r"C:\Program Files\Git\cmd\git.exe",
                            "git_version": "git version 2.49.0.windows.1",
                            "gh_found": False,
                            "gh_path": "",
                            "gh_version": "",
                            "gh_auth_ready": None,
                            "gh_auth_detail": "",
                            "winget_found": True,
                            "winget_path": r"C:\Users\danielraffel\AppData\Local\Microsoft\WindowsApps\winget.exe",
                            "winget_version": "v1.28.220",
                        },
                    ):
                        with mock.patch.object(
                            self.mod,
                            "probe_windows_repo_checkout",
                            return_value={
                                "home_dir": r"C:\Users\danielraffel",
                                "repo_path": r"C:\Users\danielraffel\pulp-validate",
                                "repo_exists": True,
                                "git_dir_exists": True,
                                "head_exists": True,
                                "setup_exists": True,
                                "origin_url": "https://github.com/danielraffel/pulp",
                                "repo_path_unsafe": False,
                            },
                        ):
                            buf = io.StringIO()
                            with mock.patch.object(self.mod, "ssh_reachable", return_value=True):
                                with redirect_stdout(buf):
                                    exit_code = self.mod.cmd_desktop_doctor(SimpleNamespace(target="windows"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop doctor for `windows`", output)
        self.assertIn("PASS  ssh: win", output)
        self.assertIn("WARN  scheduled_task: PulpDesktopAutomationAgent-windows (missing)", output)
        self.assertIn(r"PASS  interactive_user: DESKTOP\daniel", output)
        self.assertIn(r"PASS  git: git version 2.49.0.windows.1 (C:\Program Files\Git\cmd\git.exe)", output)
        self.assertIn("WARN  gh: missing; optional for remote GitHub workflows on the Windows target", output)
        self.assertIn(r"PASS  repo_checkout: C:\Users\danielraffel\pulp-validate (https://github.com/danielraffel/pulp)", output)

    def test_cmd_desktop_doctor_accepts_disconnected_windows_session(self):
        self._set_target_enabled("windows", True)
        with mock.patch.object(self.mod, "ensure_host_reachable", return_value="win2"):
            with mock.patch.object(
                self.mod,
                "bootstrap_windows_session_agent",
                return_value={
                    "task_name": "PulpDesktopAutomationAgent-windows",
                    "remote_root": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent",
                    "script_path": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\agent.ps1",
                    "script_exists": True,
                    "jobs_dir_exists": True,
                    "results_dir_exists": True,
                },
            ):
                with mock.patch.object(
                    self.mod,
                    "probe_windows_session_agent",
                    return_value={
                        "task_name": "PulpDesktopAutomationAgent-windows",
                        "task_present": True,
                        "task_state": "Ready",
                        "interactive_user": "",
                        "logged_on_user": "danielraffel",
                        "session_state": "Disc",
                        "remote_root": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent",
                        "agent_root_exists": True,
                        "jobs_dir": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\jobs",
                        "jobs_dir_exists": True,
                        "results_dir": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\results",
                        "results_dir_exists": True,
                        "script_path": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\agent.ps1",
                        "script_exists": True,
                    },
                ):
                    with mock.patch.object(
                        self.mod,
                        "ensure_windows_remote_tooling",
                        return_value={
                            "probe": {
                                "git_found": True,
                                "git_path": r"C:\Program Files\Git\cmd\git.exe",
                                "git_version": "git version 2.49.0.windows.1",
                                "gh_found": False,
                                "winget_found": True,
                            },
                            "installed": [],
                        },
                    ):
                        with mock.patch.object(
                            self.mod,
                            "ensure_windows_remote_repo_checkout",
                            return_value={
                                "home_dir": r"C:\Users\danielraffel",
                                "repo_path": r"C:\Users\danielraffel\pulp-validate",
                                "repo_exists": True,
                                "git_dir_exists": True,
                                "head_exists": True,
                                "setup_exists": True,
                                "origin_url": "https://github.com/danielraffel/pulp",
                                "repo_path_unsafe": False,
                            },
                        ):
                            with mock.patch.object(self.mod, "ssh_reachable", return_value=True):
                                with mock.patch.object(
                                    self.mod.subprocess,
                                    "run",
                                    return_value=subprocess.CompletedProcess(["git"], 0, "a" * 40 + "\n", ""),
                                ):
                                    with mock.patch.object(
                                        self.mod,
                                        "sync_job_bundle_to_ssh_host",
                                        return_value=("bundle.git", "refs/pulp-ci-bundles/install"),
                                    ):
                                        self.mod.cmd_desktop_install(SimpleNamespace(target="windows"))
                    with mock.patch.object(
                        self.mod,
                        "probe_windows_remote_tooling",
                        return_value={
                            "git_found": True,
                            "git_path": r"C:\Program Files\Git\cmd\git.exe",
                            "git_version": "git version 2.49.0.windows.1",
                            "gh_found": False,
                            "gh_path": "",
                            "gh_version": "",
                            "gh_auth_ready": None,
                            "gh_auth_detail": "",
                            "winget_found": True,
                            "winget_path": r"C:\Users\danielraffel\AppData\Local\Microsoft\WindowsApps\winget.exe",
                            "winget_version": "v1.28.220",
                        },
                    ):
                        with mock.patch.object(
                            self.mod,
                            "probe_windows_repo_checkout",
                            return_value={
                                "home_dir": r"C:\Users\danielraffel",
                                "repo_path": r"C:\Users\danielraffel\pulp-validate",
                                "repo_exists": True,
                                "git_dir_exists": True,
                                "head_exists": True,
                                "setup_exists": True,
                                "origin_url": "https://github.com/danielraffel/pulp",
                                "repo_path_unsafe": False,
                            },
                        ):
                            buf = io.StringIO()
                            with mock.patch.object(self.mod, "ssh_reachable", return_value=True):
                                with redirect_stdout(buf):
                                    exit_code = self.mod.cmd_desktop_doctor(SimpleNamespace(target="windows"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("PASS  interactive_user: danielraffel (Disc)", output)
    def test_cmd_desktop_doctor_treats_macos_accessibility_as_optional(self):
        self.mod.cmd_desktop_install(SimpleNamespace(target="mac"))

        with mock.patch.object(self.mod, "macos_accessibility_trusted", return_value=False):
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_desktop_doctor(SimpleNamespace(target="mac"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("WARN  accessibility:", output)
        self.assertIn("Pulp app automation still works", output)

    def test_cmd_desktop_status_prints_target_summary(self):
        self.mod.cmd_desktop_install(SimpleNamespace(target="mac"))

        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_desktop_status(SimpleNamespace(target="mac"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop automation:", output)
        self.assertIn("mac:", output)
        self.assertIn("adapter: macos-local", output)
        self.assertIn("installed: yes", output)
        self.assertIn("pulp_app_automation", output)

    def test_cmd_desktop_status_prints_windows_contract_summary(self):
        self._set_target_enabled("windows", True)
        with mock.patch.object(self.mod, "ensure_host_reachable", return_value="win2"):
            with mock.patch.object(
                self.mod,
                "bootstrap_windows_session_agent",
                return_value={
                    "task_name": "PulpDesktopAutomationAgent-windows",
                    "remote_root": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent",
                    "script_path": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\agent.ps1",
                    "script_exists": True,
                    "jobs_dir_exists": True,
                    "results_dir_exists": True,
                },
            ):
                with mock.patch.object(
                    self.mod,
                    "probe_windows_session_agent",
                    return_value={
                        "task_present": True,
                        "task_state": "Ready",
                        "interactive_user": r"DESKTOP\daniel",
                        "remote_root": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent",
                        "agent_root_exists": True,
                        "jobs_dir": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\jobs",
                        "jobs_dir_exists": True,
                        "results_dir": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\results",
                        "results_dir_exists": True,
                        "script_path": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\agent.ps1",
                        "script_exists": True,
                    },
                ):
                    with mock.patch.object(
                        self.mod,
                        "ensure_windows_remote_tooling",
                        return_value={
                            "probe": {
                                "git_found": True,
                                "git_path": r"C:\Program Files\Git\cmd\git.exe",
                                "git_version": "git version 2.49.0.windows.1",
                                "gh_found": True,
                                "gh_path": r"C:\Program Files\GitHub CLI\gh.exe",
                                "gh_version": "gh version 2.70.0",
                                "gh_auth_ready": True,
                                "gh_auth_detail": "authenticated",
                                "winget_found": True,
                                "winget_path": r"C:\Users\danielraffel\AppData\Local\Microsoft\WindowsApps\winget.exe",
                                "winget_version": "v1.28.220",
                            },
                            "installed": [],
                        },
                    ):
                        with mock.patch.object(
                            self.mod,
                            "ensure_windows_remote_repo_checkout",
                            return_value={
                                "home_dir": r"C:\Users\danielraffel",
                                "repo_path": r"C:\Users\danielraffel\pulp-validate",
                                "repo_exists": True,
                                "git_dir_exists": True,
                                "head_exists": True,
                                "setup_exists": True,
                                "origin_url": "https://github.com/danielraffel/pulp",
                                "repo_path_unsafe": False,
                            },
                        ):
                            with mock.patch.object(
                                self.mod.subprocess,
                                "run",
                                return_value=subprocess.CompletedProcess(["git"], 0, "a" * 40 + "\n", ""),
                            ):
                                with mock.patch.object(
                                    self.mod,
                                    "sync_job_bundle_to_ssh_host",
                                    return_value=("bundle.git", "refs/pulp-ci-bundles/install"),
                                ):
                                    self.mod.cmd_desktop_install(SimpleNamespace(target="windows"))

        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_desktop_status(SimpleNamespace(target="windows"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("task_name: PulpDesktopAutomationAgent-windows", output)
        self.assertIn(r"remote_root: C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent", output)
        self.assertIn("remote_bootstrap_ready: True", output)
        self.assertIn("remote_tooling_ready: True", output)
        self.assertIn("remote_repo_checkout_ready: True", output)
        self.assertIn(r"remote_git: git version 2.49.0.windows.1 (C:\Program Files\Git\cmd\git.exe)", output)
        self.assertIn(r"remote_gh: gh version 2.70.0 (C:\Program Files\GitHub CLI\gh.exe)", output)
        self.assertIn(r"remote_repo_checkout: C:\Users\danielraffel\pulp-validate (https://github.com/danielraffel/pulp)", output)

    def test_cmd_desktop_status_includes_latest_run(self):
        config = self.mod.load_config()
        artifact_root = Path(self.tmpdir.name) / "desktop-artifacts"
        config["desktop_automation"]["artifact_root"] = str(artifact_root)

        bundle = self.mod.create_desktop_run_bundle(config, "mac", "smoke")
        screenshot = bundle / "screenshots" / "window.png"
        screenshot.write_text("png")
        ui_snapshot = bundle / "ui-tree.json"
        ui_snapshot.write_text("{}")
        manifest = {
            "target": "mac",
            "action": "smoke",
            "label": "calculator",
            "completed_at": "2026-04-04T06:30:00+00:00",
            "interaction": {"mode": "pulp-app"},
            "artifacts": {
                "bundle_dir": str(bundle),
                "before_screenshot": str(bundle / "screenshots" / "before.png"),
                "diff_screenshot": str(bundle / "screenshots" / "diff.png"),
                "image_change": {"changed": True, "method": "pixel-bbox"},
                "screenshot": str(screenshot),
                "ui_snapshot": str(ui_snapshot),
            },
        }
        (bundle / "manifest.json").write_text(json.dumps(manifest) + "\n")
        report_dir = artifact_root / "_published" / "20260404-063100-test"
        report_dir.mkdir(parents=True, exist_ok=True)
        report_payload = {
            "generated_at": "2026-04-04T06:31:00+00:00",
            "label": "mac-gallery",
            "publish_mode": "none",
            "publish_branch": "dev-artifacts",
            "run_count": 1,
            "runs": [{"label": "calculator"}],
        }
        (report_dir / "index.json").write_text(json.dumps(report_payload) + "\n")
        (report_dir / "index.html").write_text("<html></html>\n")

        with mock.patch.object(self.mod, "load_config", return_value=config):
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_desktop_status(SimpleNamespace(target="mac"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("latest_run: calculator @ 2026-04-04T06:30:00+00:00", output)
        self.assertIn("latest_run_status: pass", output)
        self.assertIn("latest_run_source: mode=legacy sha=? branch=?", output)
        self.assertIn("latest_interaction_mode: pulp-app", output)
        self.assertIn(str(bundle / "screenshots" / "diff.png"), output)
        self.assertIn(str(screenshot), output)
        self.assertIn(str(ui_snapshot), output)

    def test_cmd_desktop_status_includes_latest_proof_when_newest_run_failed(self):
        config = self.mod.load_config()
        artifact_root = Path(self.tmpdir.name) / "desktop-artifacts"
        config["desktop_automation"]["artifact_root"] = str(artifact_root)

        proof_bundle, _ = self._write_desktop_manifest(
            config,
            "windows",
            "inspect",
            {
                "target": "windows",
                "adapter": "windows-session-agent",
                "action": "inspect",
                "label": "good-proof",
                "completed_at": "2026-04-04T06:30:00+00:00",
                "host": "win2",
                "interaction": {"mode": "pulp-app"},
                "source": {"mode": "exact-sha", "branch": "feature/ui", "sha": "a" * 40},
                "artifacts": {
                    "screenshot": "proof.png",
                    "agent_manifest": "agent.json",
                },
                "agent_status": "pass",
            },
        )
        failed_bundle, _ = self._write_desktop_manifest(
            config,
            "windows",
            "inspect",
            {
                "target": "windows",
                "adapter": "windows-session-agent",
                "action": "inspect",
                "label": "failed-run",
                "completed_at": "2026-04-04T06:31:00+00:00",
                "host": "win2",
                "source": {"mode": "live", "branch": "feature/ui", "sha": "b" * 40},
                "agent_status": "error",
            },
        )

        with mock.patch.object(self.mod, "load_config", return_value=config):
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_desktop_status(SimpleNamespace(target="windows"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("latest_run: failed-run @ 2026-04-04T06:31:00+00:00", output)
        self.assertIn("latest_run_status: error", output)
        self.assertIn("latest_proof: inspect mode=exact-sha sha=aaaaaaaaaaaa @ 2026-04-04T06:30:00+00:00", output)
        self.assertIn("latest_proof_scope: live-host host=win2 runs=1", output)
        self.assertIn(str(failed_bundle), output)
        self.assertIn(str(proof_bundle), output)

    def test_cmd_desktop_config_show_json(self):
        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_desktop_config_show(SimpleNamespace(json=True))

        payload = json.loads(buf.getvalue())
        self.assertEqual(exit_code, 0)
        self.assertIn("artifact_root", payload)
        self.assertIn("publish_mode", payload)

    def test_cmd_desktop_config_set_updates_file(self):
        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_desktop_config_set(
                SimpleNamespace(key="retention_days", value="30", json=False)
            )

        self.assertEqual(exit_code, 0)
        updated = self.mod.load_config()
        self.assertEqual(updated["desktop_automation"]["retention_days"], 30)
        self.assertIn("retention_days = 30", buf.getvalue())

    def test_cmd_desktop_config_set_target_optional_updates_file(self):
        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_desktop_config_set(
                SimpleNamespace(key="target.mac.webview_driver", value="true", json=False)
            )

        self.assertEqual(exit_code, 0)
        updated = self.mod.load_config()
        self.assertTrue(updated["desktop_automation"]["targets"]["mac"]["optional"]["webview_driver"])
        self.assertIn("target.mac.webview_driver = True", buf.getvalue())

    def test_cmd_desktop_status_reports_optional_capabilities(self):
        config = self.mod.load_config()
        config["desktop_automation"]["targets"]["mac"]["optional"]["webview_driver"] = True
        config["desktop_automation"]["targets"]["mac"]["optional"]["webdriver_url"] = "http://127.0.0.1:4444"
        config["desktop_automation"]["targets"]["mac"]["optional"]["debug_attach"] = True

        with mock.patch.object(self.mod, "load_config", return_value=config):
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_desktop_status(SimpleNamespace(target="mac", json=False))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("optional_capabilities: webview_dom, semantic_click, semantic_type, script_eval, element_screenshot, debug_attach, debug_command", output)
        self.assertIn('"webview_driver": true', output)

    def test_probe_webdriver_endpoint_uses_status_and_parses_ready(self):
        response = mock.MagicMock()
        response.read.return_value = b'{"value":{"ready":true,"message":"ok"}}'
        context = mock.MagicMock()
        context.__enter__.return_value = response
        context.__exit__.return_value = False

        with mock.patch.object(self.mod.urllib.request, "urlopen", return_value=context) as mocked_urlopen:
            probe = self.mod.probe_webdriver_endpoint("http://127.0.0.1:4444")

        self.assertEqual(probe["status_url"], "http://127.0.0.1:4444/status")
        self.assertEqual(probe["ready"], True)
        self.assertEqual(probe["message"], "ok")
        request = mocked_urlopen.call_args.args[0]
        self.assertEqual(urlparse(request.full_url).path, "/status")

    def test_desktop_doctor_reports_optional_webview_driver_and_debug_attach(self):
        config = self.mod.load_config()
        config["desktop_automation"]["targets"]["mac"]["optional"]["webview_driver"] = True
        config["desktop_automation"]["targets"]["mac"]["optional"]["webdriver_url"] = "http://127.0.0.1:4444"
        config["desktop_automation"]["targets"]["mac"]["optional"]["debug_attach"] = True
        config["desktop_automation"]["targets"]["mac"]["optional"]["debugger_command"] = "lldb"
        original_platform = self.mod.sys.platform
        self.mod.sys.platform = "darwin"
        try:
            with mock.patch.object(self.mod, "macos_accessibility_trusted", return_value=True):
                with mock.patch.object(self.mod, "probe_webdriver_endpoint", return_value={"status_url": "http://127.0.0.1:4444/status", "ready": True, "message": "ok"}):
                    with mock.patch.object(self.mod.shutil, "which", side_effect=lambda cmd: f"/usr/bin/{cmd}"):
                        checks = self.mod.desktop_doctor_checks(config, "mac")
        finally:
            self.mod.sys.platform = original_platform

        names = {check["name"]: check for check in checks}
        self.assertEqual(names["webview_driver"]["detail"], "reachable at http://127.0.0.1:4444/status (ready=true): ok")
        self.assertTrue(names["webview_driver"]["ok"])
        self.assertTrue(names["debug_attach"]["ok"])

    def test_cmd_desktop_recent_lists_recent_manifests(self):
        config = self.mod.load_config()
        artifact_root = Path(self.tmpdir.name) / "desktop-artifacts"
        config["desktop_automation"]["artifact_root"] = str(artifact_root)

        first_bundle = self.mod.create_desktop_run_bundle(config, "mac", "smoke")
        second_bundle = self.mod.create_desktop_run_bundle(config, "mac", "smoke")
        first_manifest = {
            "target": "mac",
            "action": "smoke",
            "label": "older",
            "completed_at": "2026-04-04T06:30:00+00:00",
            "artifacts": {"bundle_dir": str(first_bundle)},
        }
        second_manifest = {
            "target": "mac",
            "action": "smoke",
            "label": "newer",
            "completed_at": "2026-04-04T06:31:00+00:00",
            "artifacts": {"bundle_dir": str(second_bundle)},
        }
        (first_bundle / "manifest.json").write_text(json.dumps(first_manifest) + "\n")
        (second_bundle / "manifest.json").write_text(json.dumps(second_manifest) + "\n")

        with mock.patch.object(self.mod, "load_config", return_value=config):
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_desktop_recent(SimpleNamespace(target="mac", action="smoke", limit=5))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop automation recent runs:", output)
        self.assertTrue(output.index("newer") < output.index("older"))

    def test_cmd_desktop_status_json_emits_machine_readable_payload(self):
        config = self.mod.load_config()
        artifact_root = Path(self.tmpdir.name) / "desktop-artifacts"
        config["desktop_automation"]["artifact_root"] = str(artifact_root)

        bundle = self.mod.create_desktop_run_bundle(config, "mac", "smoke")
        manifest = {
            "target": "mac",
            "action": "smoke",
            "label": "calculator",
            "completed_at": "2026-04-04T06:30:00+00:00",
            "interaction": {"mode": "pulp-app"},
            "artifacts": {
                "bundle_dir": str(bundle),
                "before_screenshot": str(bundle / "screenshots" / "before.png"),
                "diff_screenshot": str(bundle / "screenshots" / "diff.png"),
                "image_change": {"changed": True, "method": "pixel-bbox"},
                "screenshot": str(bundle / "screenshots" / "window.png"),
                "ui_snapshot": str(bundle / "ui-tree.json"),
            },
        }
        (bundle / "manifest.json").write_text(json.dumps(manifest) + "\n")
        report_dir = artifact_root / "_published" / "20260404-063100-test"
        report_dir.mkdir(parents=True, exist_ok=True)
        report_payload = {
            "generated_at": "2026-04-04T06:31:00+00:00",
            "label": "mac-gallery",
            "publish_mode": "none",
            "publish_branch": "dev-artifacts",
            "run_count": 1,
            "runs": [{"label": "calculator"}],
        }
        (report_dir / "index.json").write_text(json.dumps(report_payload) + "\n")
        (report_dir / "index.html").write_text("<html></html>\n")


        with mock.patch.object(self.mod, "load_config", return_value=config):
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_desktop_status(SimpleNamespace(target="mac", json=True))

        payload = json.loads(buf.getvalue())
        self.assertEqual(exit_code, 0)
        self.assertEqual(payload["targets"][0]["name"], "mac")
        self.assertEqual(payload["targets"][0]["latest_run"]["label"], "calculator")
        self.assertEqual(payload["targets"][0]["latest_run"]["interaction_mode"], "pulp-app")
        self.assertEqual(payload["targets"][0]["latest_run"]["ui_snapshot"], str(bundle / "ui-tree.json"))
        self.assertEqual(payload["targets"][0]["latest_run"]["run_status"], "pass")
        self.assertEqual(payload["targets"][0]["latest_run"]["source_mode"], "legacy")
        self.assertEqual(payload["targets"][0]["latest_proof"]["source"]["mode"], "legacy")
        self.assertEqual(payload["latest_publish"]["label"], "mac-gallery")
        self.assertTrue(payload["latest_publish"]["index_html"].endswith("index.html"))

    def test_cmd_desktop_recent_json_emits_runs(self):
        config = self.mod.load_config()
        artifact_root = Path(self.tmpdir.name) / "desktop-artifacts"
        config["desktop_automation"]["artifact_root"] = str(artifact_root)

        bundle = self.mod.create_desktop_run_bundle(config, "mac", "smoke")
        manifest = {
            "target": "mac",
            "action": "smoke",
            "label": "calculator",
            "completed_at": "2026-04-04T06:30:00+00:00",
            "interaction": {"mode": "pulp-app"},
            "artifacts": {
                "bundle_dir": str(bundle),
                "before_screenshot": str(bundle / "screenshots" / "before.png"),
                "diff_screenshot": str(bundle / "screenshots" / "diff.png"),
                "image_change": {"changed": True, "method": "pixel-bbox"},
            },
        }
        (bundle / "manifest.json").write_text(json.dumps(manifest) + "\n")

        with mock.patch.object(self.mod, "load_config", return_value=config):
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_desktop_recent(SimpleNamespace(target="mac", action="smoke", limit=5, json=True))

        payload = json.loads(buf.getvalue())
        self.assertEqual(exit_code, 0)
        self.assertEqual(payload["runs"][0]["label"], "calculator")
        self.assertEqual(payload["runs"][0]["interaction"]["mode"], "pulp-app")

    def test_cmd_desktop_recent_prints_run_status_source_and_scope(self):
        config = self.mod.load_config()
        artifact_root = Path(self.tmpdir.name) / "desktop-artifacts"
        config["desktop_automation"]["artifact_root"] = str(artifact_root)

        self._write_desktop_manifest(
            config,
            "windows",
            "inspect",
            {
                "target": "windows",
                "adapter": "windows-session-agent",
                "action": "inspect",
                "label": "good-proof",
                "completed_at": "2026-04-04T06:30:00+00:00",
                "host": "win2",
                "source": {"mode": "exact-sha", "branch": "feature/ui", "sha": "a" * 40},
                "agent_status": "pass",
            },
        )

        with mock.patch.object(self.mod, "load_config", return_value=config):
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_desktop_recent(SimpleNamespace(target="windows", action="inspect", limit=5))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("status: pass", output)
        self.assertIn("source: mode=exact-sha sha=aaaaaaaaaaaa branch=feature/ui", output)
        self.assertIn("proof_scope: live-host host=win2", output)

    def test_cmd_desktop_proof_groups_latest_success_by_target_action_source_and_sha(self):
        config = self.mod.load_config()
        artifact_root = Path(self.tmpdir.name) / "desktop-artifacts"
        config["desktop_automation"]["artifact_root"] = str(artifact_root)

        self._write_desktop_manifest(
            config,
            "windows",
            "inspect",
            {
                "target": "windows",
                "adapter": "windows-session-agent",
                "action": "inspect",
                "label": "older-pass",
                "completed_at": "2026-04-04T06:30:00+00:00",
                "host": "win2",
                "interaction": {"mode": "pulp-app"},
                "source": {"mode": "exact-sha", "branch": "feature/ui", "sha": "a" * 40},
                "agent_status": "pass",
            },
        )
        self._write_desktop_manifest(
            config,
            "windows",
            "inspect",
            {
                "target": "windows",
                "adapter": "windows-session-agent",
                "action": "inspect",
                "label": "newer-pass",
                "completed_at": "2026-04-04T06:31:00+00:00",
                "host": "win2",
                "interaction": {"mode": "pulp-app"},
                "source": {"mode": "exact-sha", "branch": "feature/ui", "sha": "a" * 40},
                "agent_status": "pass",
            },
        )
        self._write_desktop_manifest(
            config,
            "windows",
            "inspect",
            {
                "target": "windows",
                "adapter": "windows-session-agent",
                "action": "inspect",
                "label": "failed-run",
                "completed_at": "2026-04-04T06:32:00+00:00",
                "host": "win2",
                "source": {"mode": "exact-sha", "branch": "feature/ui", "sha": "a" * 40},
                "agent_status": "error",
            },
        )
        self._write_desktop_manifest(
            config,
            "windows",
            "inspect",
            {
                "target": "windows",
                "adapter": "windows-session-agent",
                "action": "inspect",
                "label": "legacy-pass",
                "completed_at": "2026-04-04T06:29:00+00:00",
                "host": "win2",
                "agent_status": "pass",
            },
        )

        with mock.patch.object(self.mod, "load_config", return_value=config):
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_desktop_proof(
                    SimpleNamespace(
                        target="windows",
                        action="inspect",
                        source_mode="exact-sha",
                        sha=None,
                        branch=None,
                        limit=10,
                        json=False,
                    )
                )

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("windows/inspect: mode=exact-sha sha=aaaaaaaaaaaa @ 2026-04-04T06:31:00+00:00", output)
        self.assertIn("proof_scope: live-host adapter=windows-session-agent host=win2 runs=2", output)
        self.assertIn("label: newer-pass", output)
        self.assertNotIn("failed-run", output)
        self.assertNotIn("legacy-pass", output)

    def test_cmd_desktop_proof_json_emits_grouped_proofs(self):
        config = self.mod.load_config()
        artifact_root = Path(self.tmpdir.name) / "desktop-artifacts"
        config["desktop_automation"]["artifact_root"] = str(artifact_root)

        bundle, _ = self._write_desktop_manifest(
            config,
            "mac",
            "inspect",
            {
                "target": "mac",
                "adapter": "macos-local",
                "action": "inspect",
                "label": "preview",
                "completed_at": "2026-04-04T06:30:00+00:00",
                "interaction": {"mode": "pulp-app"},
                "source": {"mode": "legacy", "branch": None, "sha": None},
                "artifacts": {"screenshot": "window.png"},
                "status": "pass",
            },
        )

        with mock.patch.object(self.mod, "load_config", return_value=config):
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_desktop_proof(
                    SimpleNamespace(
                        target="mac",
                        action="inspect",
                        source_mode="legacy",
                        sha=None,
                        branch=None,
                        limit=10,
                        json=True,
                    )
                )

        payload = json.loads(buf.getvalue())
        self.assertEqual(exit_code, 0)
        self.assertEqual(payload["proofs"][0]["target"], "mac")
        self.assertEqual(payload["proofs"][0]["source"]["mode"], "legacy")
        self.assertEqual(payload["proofs"][0]["latest_run"]["artifacts"]["bundle_dir"], str(bundle))

    def test_write_desktop_run_rollups_writes_latest_run_latest_proof_and_jsonl(self):
        config = self.mod.load_config()
        artifact_root = Path(self.tmpdir.name) / "desktop-artifacts"
        config["desktop_automation"]["artifact_root"] = str(artifact_root)

        self._write_desktop_manifest(
            config,
            "windows",
            "inspect",
            {
                "target": "windows",
                "adapter": "windows-session-agent",
                "action": "inspect",
                "label": "good-proof",
                "completed_at": "2026-04-04T06:30:00+00:00",
                "source": {"mode": "exact-sha", "branch": "feature/test", "sha": "a" * 40},
                "artifacts": {"screenshot": "good.png"},
                "status": "pass",
            },
        )
        self._write_desktop_manifest(
            config,
            "windows",
            "inspect",
            {
                "target": "windows",
                "adapter": "windows-session-agent",
                "action": "inspect",
                "label": "failed-run",
                "completed_at": "2026-04-04T06:31:00+00:00",
                "source": {"mode": "exact-sha", "branch": "feature/test", "sha": "b" * 40},
                "artifacts": {"screenshot": "bad.png"},
                "status": "error",
            },
        )

        self.mod.write_desktop_run_rollups(config, target_name="windows")
        self.mod.write_desktop_run_rollups(config)

        target_root = artifact_root / "windows"
        latest_run = json.loads((target_root / "latest-run.json").read_text())
        latest_proof = json.loads((target_root / "latest-proof.json").read_text())
        runs_jsonl = (target_root / "runs.jsonl").read_text().strip().splitlines()
        overall_latest = json.loads((artifact_root / "latest-run.json").read_text())

        self.assertEqual(latest_run["label"], "failed-run")
        self.assertEqual(latest_run["run_status"], "error")
        self.assertEqual(latest_proof["latest_run"]["label"], "good-proof")
        self.assertEqual(latest_proof["source"]["sha"], "a" * 40)
        self.assertEqual(len(runs_jsonl), 2)
        self.assertEqual(overall_latest["label"], "failed-run")

    def test_stage_desktop_publish_report_copies_artifacts_and_writes_indexes(self):
        config = self.mod.load_config()
        artifact_root = Path(self.tmpdir.name) / "desktop-artifacts"
        config["desktop_automation"]["artifact_root"] = str(artifact_root)

        bundle = self.mod.create_desktop_run_bundle(config, "mac", "click")
        screenshot = bundle / "screenshots" / "window.png"
        before = bundle / "screenshots" / "before.png"
        diff = bundle / "screenshots" / "diff.png"
        ui_snapshot = bundle / "ui-tree.json"
        stdout_log = bundle / "stdout.log"
        stderr_log = bundle / "stderr.log"
        for path, content in (
            (screenshot, b"after"),
            (before, b"before"),
            (diff, b"diff"),
        ):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        ui_snapshot.write_text('{"type":"View"}')
        stdout_log.write_text("stdout")
        stderr_log.write_text("stderr")
        manifest = {
            "target": "mac",
            "action": "click",
            "label": "ui-preview",
            "completed_at": "2026-04-04T06:30:00+00:00",
            "artifacts": {
                "bundle_dir": str(bundle),
                "screenshot": str(screenshot),
                "before_screenshot": str(before),
                "diff_screenshot": str(diff),
                "ui_snapshot": str(ui_snapshot),
                "stdout": str(stdout_log),
                "stderr": str(stderr_log),
                "image_change": {"changed": True, "method": "pixel-bbox"},
            },
            "interaction": {"mode": "pulp-app"},
        }
        (bundle / "manifest.json").write_text(json.dumps(manifest) + "\n")

        report = self.mod.stage_desktop_publish_report(config, [manifest], label="desktop-gallery")

        output_dir = Path(report["output_dir"])
        self.assertTrue((output_dir / "index.html").exists())
        self.assertTrue((output_dir / "index.json").exists())
        payload = json.loads((output_dir / "index.json").read_text())
        self.assertEqual(payload["label"], "desktop-gallery")
        self.assertEqual(payload["run_count"], 1)
        latest_report = json.loads((artifact_root / "_published" / "latest-report.json").read_text())
        reports_jsonl = (artifact_root / "_published" / "reports.jsonl").read_text().strip().splitlines()
        self.assertEqual(latest_report["label"], "desktop-gallery")
        self.assertEqual(len(reports_jsonl), 1)
        published_run = payload["runs"][0]
        self.assertEqual(published_run["artifacts"]["image_change"]["method"], "pixel-bbox")
        self.assertTrue((output_dir / published_run["artifacts"]["screenshot"]).exists())
        html_text = (output_dir / "index.html").read_text()
        self.assertIn("ui-preview", html_text)
        self.assertIn("window.png", html_text)

    def test_normalize_git_remote_for_http_handles_github_urls(self):
        self.assertEqual(
            self.mod.normalize_git_remote_for_http('git@github.com:danielraffel/pulp.git'),
            'https://github.com/danielraffel/pulp',
        )
        self.assertEqual(
            self.mod.normalize_git_remote_for_http('https://github.com/danielraffel/pulp.git'),
            'https://github.com/danielraffel/pulp',
        )
        self.assertIsNone(self.mod.normalize_git_remote_for_http('/tmp/pulp.git'))

    def test_normalize_git_remote_for_clone_handles_github_urls(self):
        self.assertEqual(
            self.mod.normalize_git_remote_for_clone('git@github.com:danielraffel/pulp.git'),
            'https://github.com/danielraffel/pulp.git',
        )
        self.assertEqual(
            self.mod.normalize_git_remote_for_clone('https://github.com/danielraffel/pulp'),
            'https://github.com/danielraffel/pulp.git',
        )
        self.assertEqual(
            self.mod.normalize_git_remote_for_clone('https://github.com/danielraffel/pulp.git'),
            'https://github.com/danielraffel/pulp.git',
        )
        self.assertIsNone(self.mod.normalize_git_remote_for_clone('/tmp/pulp.git'))

    def test_publish_report_to_branch_pushes_report_to_remote_branch(self):
        config = self.mod.load_config()
        artifact_root = Path(self.tmpdir.name) / 'desktop-artifacts'
        config['desktop_automation']['artifact_root'] = str(artifact_root)
        config['desktop_automation']['publish_mode'] = 'branch'
        config['desktop_automation']['publish_branch'] = 'dev-artifacts-test'

        report_dir = artifact_root / '_published' / '20260404-branch-test'
        (report_dir / 'assets' / 'run-01').mkdir(parents=True, exist_ok=True)
        (report_dir / 'assets' / 'run-01' / 'window.png').write_bytes(b'png')
        (report_dir / 'index.json').write_text(json.dumps({'label': 'branch-gallery'}) + '\n')
        (report_dir / 'index.html').write_text('<html></html>\n')
        report = {
            'generated_at': '2026-04-04T21:30:00+00:00',
            'label': 'branch-gallery',
            'publish_mode': 'branch',
            'publish_branch': 'dev-artifacts-test',
            'output_dir': str(report_dir),
            'index_html': str(report_dir / 'index.html'),
            'index_json': str(report_dir / 'index.json'),
            'run_count': 1,
            'runs': [
                {
                    'label': 'ui-preview',
                    'target': 'mac',
                    'action': 'click',
                    'artifacts': {'screenshot': 'assets/run-01/window.png'},
                }
            ],
        }

        git_root = Path(self.tmpdir.name) / 'git-root'
        remote_root = Path(self.tmpdir.name) / 'remote.git'
        git_root.mkdir(parents=True, exist_ok=True)
        subprocess.run(['git', 'init', '--bare', str(remote_root)], check=True, capture_output=True, text=True)
        subprocess.run(['git', 'init'], cwd=git_root, check=True, capture_output=True, text=True)
        subprocess.run(['git', 'config', 'user.name', 'Pulp Tests'], cwd=git_root, check=True, capture_output=True, text=True)
        subprocess.run(['git', 'config', 'user.email', 'tests@example.com'], cwd=git_root, check=True, capture_output=True, text=True)
        (git_root / 'README.md').write_text('root\n')
        subprocess.run(['git', 'add', 'README.md'], cwd=git_root, check=True, capture_output=True, text=True)
        subprocess.run(['git', 'commit', '-m', 'Initial'], cwd=git_root, check=True, capture_output=True, text=True)
        subprocess.run(['git', 'remote', 'add', 'origin', str(remote_root)], cwd=git_root, check=True, capture_output=True, text=True)

        with mock.patch.object(self.mod, 'ROOT', git_root):
            published = self.mod.publish_report_to_branch(config, report)

        self.assertEqual(published['mode'], 'branch')
        self.assertEqual(published['branch'], 'dev-artifacts-test')
        clone_root = Path(self.tmpdir.name) / 'clone'
        subprocess.run(['git', 'clone', '--branch', 'dev-artifacts-test', str(remote_root), str(clone_root)], check=True, capture_output=True, text=True)
        self.assertTrue((clone_root / 'desktop-automation' / 'reports' / '20260404-branch-test' / 'index.json').exists())
        self.assertTrue((clone_root / 'desktop-automation' / 'latest' / 'index.html').exists())

    def test_stage_desktop_publish_report_includes_branch_publish_metadata_when_enabled(self):
        config = self.mod.load_config()
        artifact_root = Path(self.tmpdir.name) / 'desktop-artifacts'
        config['desktop_automation']['artifact_root'] = str(artifact_root)
        config['desktop_automation']['publish_mode'] = 'branch'
        bundle = self.mod.create_desktop_run_bundle(config, 'mac', 'inspect')
        screenshot = bundle / 'screenshots' / 'window.png'
        screenshot.parent.mkdir(parents=True, exist_ok=True)
        screenshot.write_bytes(b'after')
        manifest = {
            'target': 'mac',
            'action': 'inspect',
            'label': 'ui-preview',
            'completed_at': '2026-04-04T06:30:00+00:00',
            'artifacts': {'bundle_dir': str(bundle), 'screenshot': str(screenshot)},
        }
        (bundle / 'manifest.json').write_text(json.dumps(manifest) + '\n')

        with mock.patch.object(self.mod, 'publish_report_to_branch', return_value={'mode': 'branch', 'branch': 'dev-artifacts'}):
            report = self.mod.stage_desktop_publish_report(config, [manifest], label='desktop-gallery')

        self.assertEqual(report['published']['branch'], 'dev-artifacts')

    def test_cmd_desktop_publish_json_emits_report_paths(self):
        config = self.mod.load_config()
        report = {
            "output_dir": "/tmp/publish",
            "index_html": "/tmp/publish/index.html",
            "index_json": "/tmp/publish/index.json",
            "run_count": 1,
            "runs": [],
        }
        manifests = [{"label": "ui-preview"}]

        with mock.patch.object(self.mod, "load_config", return_value=config):
            with mock.patch.object(self.mod, "desktop_run_manifests", return_value=manifests):
                with mock.patch.object(self.mod, "stage_desktop_publish_report", return_value=report):
                    buf = io.StringIO()
                    with redirect_stdout(buf):
                        exit_code = self.mod.cmd_desktop_publish(
                            SimpleNamespace(target="mac", action="click", limit=3, label="gallery", output=None, json=True)
                        )

        payload = json.loads(buf.getvalue())
        self.assertEqual(exit_code, 0)
        self.assertEqual(payload["index_html"], "/tmp/publish/index.html")
        self.assertEqual(payload["run_count"], 1)

    def test_cmd_desktop_cleanup_removes_old_bundles(self):
        config = self.mod.load_config()
        artifact_root = Path(self.tmpdir.name) / "desktop-artifacts"
        config["desktop_automation"]["artifact_root"] = str(artifact_root)

        keep_bundle = self.mod.create_desktop_run_bundle(config, "mac", "smoke")
        remove_bundle = self.mod.create_desktop_run_bundle(config, "mac", "smoke")
        keep_manifest = {
            "target": "mac",
            "action": "smoke",
            "label": "keep",
            "completed_at": "2026-04-04T06:31:00+00:00",
            "artifacts": {"bundle_dir": str(keep_bundle)},
        }
        remove_manifest = {
            "target": "mac",
            "action": "smoke",
            "label": "remove",
            "completed_at": "2026-04-01T06:31:00+00:00",
            "artifacts": {"bundle_dir": str(remove_bundle)},
        }
        (keep_bundle / "manifest.json").write_text(json.dumps(keep_manifest) + "\n")
        (remove_bundle / "manifest.json").write_text(json.dumps(remove_manifest) + "\n")

        original_time = self.mod.time.time
        self.mod.time.time = lambda: self.mod.datetime.fromisoformat("2026-04-04T06:31:00+00:00").timestamp()
        try:
            with mock.patch.object(self.mod, "load_config", return_value=config):
                buf = io.StringIO()
                with redirect_stdout(buf):
                    exit_code = self.mod.cmd_desktop_cleanup(
                        SimpleNamespace(target="mac", older_than_days=1, keep_last=1)
                    )
        finally:
            self.mod.time.time = original_time

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop cleanup removed 1 bundle(s).", output)
        self.assertTrue(keep_bundle.exists())
        self.assertFalse(remove_bundle.exists())
        latest_run = json.loads((artifact_root / "mac" / "latest-run.json").read_text())
        runs_jsonl = (artifact_root / "mac" / "runs.jsonl").read_text().strip().splitlines()
        self.assertEqual(latest_run["label"], "keep")
        self.assertEqual(len(runs_jsonl), 1)

    def test_run_macos_local_smoke_writes_manifest(self):
        config = self.mod.load_config()
        config["desktop_automation"]["artifact_root"] = str(Path(self.tmpdir.name) / "desktop-artifacts")

        class FakeProc:
            def __init__(self):
                self.pid = 4242
                self._returncode = None

            def poll(self):
                return self._returncode

            def terminate(self):
                self._returncode = 0

            def wait(self, timeout=None):
                self._returncode = 0
                return 0

            def kill(self):
                self._returncode = -9

        fake_proc = FakeProc()
        screenshot_path = Path(self.tmpdir.name) / "capture.png"

        with mock.patch.object(self.mod.subprocess, "Popen", return_value=fake_proc):
            with mock.patch.object(
                self.mod,
                "wait_for_macos_window",
                return_value={"windowId": 77, "title": "Smoke", "bounds": {"x": 0, "y": 0, "width": 100, "height": 100}},
            ):
                with mock.patch.object(self.mod, "capture_macos_window") as capture:
                    manifest = self.mod.run_macos_local_smoke(
                        config,
                        "/tmp/fake-binary --flag",
                        action_name="smoke",
                        bundle_id=None,
                        label="calculator",
                        output_path=str(screenshot_path),
                        capture_ui_snapshot=False,
                        click_point=None,
                        click_view_id=None,
                        click_view_type=None,
                        click_view_text=None,
                        click_view_label=None,
                        capture_before=False,
                        settle_secs=0.5,
                        timeout_secs=1.0,
                    )

        capture.assert_called_once_with(77, screenshot_path)
        self.assertEqual(manifest["pid"], 4242)
        self.assertEqual(manifest["label"], "calculator")
        self.assertEqual(manifest["artifacts"]["screenshot"], str(screenshot_path))
        manifest_path = Path(manifest["artifacts"]["bundle_dir"]) / "manifest.json"
        self.assertTrue(manifest_path.exists())

    def test_run_macos_local_smoke_exact_sha_records_source_and_launch_cwd(self):
        config = self.mod.load_config()
        config["desktop_automation"]["artifact_root"] = str(Path(self.tmpdir.name) / "desktop-artifacts")

        class FakeProc:
            def __init__(self):
                self.pid = 5252
                self._returncode = None

            def poll(self):
                return self._returncode

            def terminate(self):
                self._returncode = 0

            def wait(self, timeout=None):
                self._returncode = 0
                return 0

            def kill(self):
                self._returncode = -9

        fake_proc = FakeProc()
        prepared_root = Path(self.tmpdir.name) / "prepared-root"
        source_context = {
            "mode": "exact-sha",
            "branch": "feature/source",
            "sha": "b" * 40,
            "prepare_command": "./scripts/build-ui-preview.sh",
            "prepare_timeout_secs": 120.0,
            "prepared_root": str(prepared_root),
            "launch_cwd": str(prepared_root),
            "launch_command": str(prepared_root / "build" / "ui-preview"),
            "prepare_log": str(Path(self.tmpdir.name) / "prepare.log"),
            "prepared_state": "clean",
        }

        with mock.patch.object(self.mod, "prepare_macos_exact_sha_source", return_value=source_context):
            with mock.patch.object(self.mod.subprocess, "Popen", return_value=fake_proc) as popen_mock:
                with mock.patch.object(
                    self.mod,
                    "wait_for_macos_window",
                    return_value={"windowId": 91, "title": "Prepared", "bounds": {"x": 0, "y": 0, "width": 100, "height": 100}},
                ):
                    with mock.patch.object(self.mod, "capture_macos_window"):
                        manifest = self.mod.run_macos_local_smoke(
                            config,
                            str(self.mod.ROOT / "build" / "ui-preview"),
                            action_name="smoke",
                            bundle_id=None,
                            label="prepared-ui",
                            output_path=None,
                            capture_ui_snapshot=False,
                            click_point=None,
                            click_view_id=None,
                            click_view_type=None,
                            click_view_text=None,
                            click_view_label=None,
                            pulp_app_automation=False,
                            capture_before=False,
                            settle_secs=0.0,
                            timeout_secs=5.0,
                            source_request={
                                "mode": "exact-sha",
                                "branch": "feature/source",
                                "sha": "b" * 40,
                                "prepare_command": "./scripts/build-ui-preview.sh",
                                "prepare_timeout_secs": 120.0,
                            },
                        )

        self.assertEqual(popen_mock.call_args.kwargs["cwd"], str(prepared_root))
        self.assertEqual(manifest["source"]["mode"], "exact-sha")
        self.assertEqual(manifest["source"]["sha"], "b" * 40)
        self.assertEqual(manifest["source"]["launch_cwd"], str(prepared_root))
        self.assertTrue(manifest["artifacts"]["prepare_log"].endswith("prepare.log"))

    def test_run_macos_local_smoke_captures_requested_ui_snapshot(self):
        config = self.mod.load_config()
        config["desktop_automation"]["artifact_root"] = str(Path(self.tmpdir.name) / "desktop-artifacts")

        class FakeProc:
            def __init__(self):
                self.pid = 4343
                self._returncode = None

            def poll(self):
                return self._returncode

            def terminate(self):
                self._returncode = 0

            def wait(self, timeout=None):
                self._returncode = 0
                return 0

            def kill(self):
                self._returncode = -9

        fake_proc = FakeProc()
        screenshot_path = Path(self.tmpdir.name) / "capture-ui.png"

        def fake_wait_for_path(path, timeout_secs):
            path.write_text(json.dumps({"id": "root", "type": "View", "children": [{"id": "gain", "type": "Knob"}]}))
            return path

        with mock.patch.object(self.mod.subprocess, "Popen", return_value=fake_proc) as popen_mock:
            with mock.patch.object(
                self.mod,
                "wait_for_macos_window",
                return_value={"windowId": 77, "title": "Smoke", "bounds": {"x": 0, "y": 0, "width": 100, "height": 100}},
            ):
                with mock.patch.object(self.mod, "wait_for_path", side_effect=fake_wait_for_path):
                    with mock.patch.object(self.mod, "capture_macos_window") as capture:
                        manifest = self.mod.run_macos_local_smoke(
                            config,
                            "/tmp/fake-binary --flag",
                            action_name="smoke",
                            bundle_id=None,
                            label="ui-preview",
                            output_path=str(screenshot_path),
                            capture_ui_snapshot=True,
                            click_point=None,
                            click_view_id=None,
                            click_view_type=None,
                            click_view_text=None,
                            click_view_label=None,
                            capture_before=False,
                            settle_secs=0.5,
                            timeout_secs=1.0,
                        )

        env = popen_mock.call_args.kwargs["env"]
        self.assertIn("PULP_VIEW_TREE_OUT", env)
        capture.assert_called_once_with(77, screenshot_path)
        self.assertEqual(manifest["artifacts"]["ui_snapshot"], env["PULP_VIEW_TREE_OUT"])
        self.assertEqual(manifest["inspector"]["view_count"], 2)
        self.assertEqual(manifest["inspector"]["root_id"], "root")

    def test_run_macos_local_smoke_can_delegate_interaction_to_pulp_app(self):
        config = self.mod.load_config()
        config["desktop_automation"]["artifact_root"] = str(Path(self.tmpdir.name) / "desktop-artifacts")

        class FakeProc:
            def __init__(self):
                self.pid = 4344
                self._returncode = None

            def poll(self):
                return self._returncode

            def terminate(self):
                self._returncode = 0

            def wait(self, timeout=None):
                self._returncode = 0
                return 0

            def kill(self):
                self._returncode = -9

        fake_proc = FakeProc()
        screenshot_path = Path(self.tmpdir.name) / "capture-ui-after.png"
        window = {"windowId": 78, "title": "Smoke", "bounds": {"x": 0, "y": 0, "width": 100, "height": 100}}

        def fake_wait_for_path(path, timeout_secs):
            if path.suffix == ".json":
                path.write_text(json.dumps({"id": "root", "type": "View", "children": [{"id": "bypass-toggle", "type": "Toggle"}]}))
            else:
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(b"png")
            return path

        with mock.patch.object(self.mod.subprocess, "Popen", return_value=fake_proc) as popen_mock:
            with mock.patch.object(self.mod, "wait_for_macos_window", return_value=window):
                with mock.patch.object(self.mod, "wait_for_path", side_effect=fake_wait_for_path):
                    with mock.patch.object(self.mod, "capture_macos_window") as capture_mock:
                        with mock.patch.object(self.mod, "dispatch_macos_click") as click_mock:
                            with mock.patch.object(self.mod, "macos_accessibility_trusted", side_effect=AssertionError("should not need Accessibility")):
                                manifest = self.mod.run_macos_local_smoke(
                                    config,
                                    "/tmp/pulp-ui-preview",
                                    action_name="smoke",
                                    bundle_id=None,
                                    label="ui-preview",
                                    output_path=str(screenshot_path),
                                    capture_ui_snapshot=True,
                                    click_point=None,
                                    click_view_id="bypass-toggle",
                                    click_view_type=None,
                                    click_view_text=None,
                                    click_view_label=None,
                                    pulp_app_automation=True,
                                    capture_before=True,
                                    settle_secs=0.75,
                                    timeout_secs=1.0,
                                )

        env = popen_mock.call_args.kwargs["env"]
        self.assertEqual(env["PULP_AUTOMATION_CLICK_VIEW_ID"], "bypass-toggle")
        self.assertEqual(env["PULP_AUTOMATION_AFTER_OUT"], str(screenshot_path))
        self.assertEqual(env["PULP_AUTOMATION_BEFORE_OUT"].endswith("before.png"), True)
        self.assertEqual(env["PULP_AUTOMATION_AFTER_DELAY_MS"], "750")
        capture_mock.assert_not_called()
        click_mock.assert_not_called()
        self.assertEqual(manifest["interaction"]["mode"], "pulp-app")
        self.assertEqual(manifest["artifacts"]["screenshot"], str(screenshot_path))
        self.assertEqual(manifest["inspector"]["view_count"], 2)

    def test_run_macos_local_smoke_rejects_ui_snapshot_for_bundle_launch(self):
        config = self.mod.load_config()
        config["desktop_automation"]["artifact_root"] = str(Path(self.tmpdir.name) / "desktop-artifacts")

        with self.assertRaises(RuntimeError):
            self.mod.run_macos_local_smoke(
                config,
                None,
                action_name="smoke",
                bundle_id="com.apple.calculator",
                label="calculator",
                output_path=str(Path(self.tmpdir.name) / "bundle.png"),
                capture_ui_snapshot=True,
                click_point=None,
                click_view_id=None,
                click_view_type=None,
                click_view_text=None,
                click_view_label=None,
                capture_before=False,
                settle_secs=0.5,
                timeout_secs=1.0,
            )

    def test_run_macos_local_smoke_launches_app_bundle_via_open(self):
        config = self.mod.load_config()
        config["desktop_automation"]["artifact_root"] = str(Path(self.tmpdir.name) / "desktop-artifacts")
        app_bundle = Path(self.tmpdir.name) / "Calculator.app"
        app_exec = app_bundle / "Contents" / "MacOS" / "Calculator"
        app_exec.parent.mkdir(parents=True, exist_ok=True)
        app_exec.write_text("")
        info_plist = app_bundle / "Contents" / "Info.plist"
        info_plist.write_bytes(
            self.mod.plistlib.dumps(
                {
                    "CFBundleIdentifier": "com.example.calculator",
                }
            )
        )
        screenshot_path = Path(self.tmpdir.name) / "bundle-open.png"

        with mock.patch.object(self.mod.subprocess, "run") as run_mock:
            with mock.patch.object(
                self.mod,
                "wait_for_macos_bundle_window",
                return_value=(5151, {"windowId": 88, "title": "Calculator", "bounds": {"x": 0, "y": 0, "width": 100, "height": 100}}),
            ):
                with mock.patch.object(self.mod, "capture_macos_window") as capture:
                    manifest = self.mod.run_macos_local_smoke(
                        config,
                        str(app_exec),
                        action_name="smoke",
                        bundle_id=None,
                        label="calculator",
                        output_path=str(screenshot_path),
                        capture_ui_snapshot=False,
                        click_point=None,
                        click_view_id=None,
                        click_view_type=None,
                        click_view_text=None,
                        click_view_label=None,
                        capture_before=False,
                        settle_secs=0.5,
                        timeout_secs=1.0,
                    )

        run_mock.assert_any_call(["open", "-a", str(app_bundle)], capture_output=True, text=True, check=True)
        capture.assert_called_once_with(88, screenshot_path)
        self.assertEqual(manifest["pid"], 5151)
        self.assertEqual(manifest["bundle_id"], "com.example.calculator")
        self.assertEqual(manifest["app_path"], str(app_bundle))

    def test_run_windows_session_agent_action_writes_manifest(self):
        self._set_target_enabled("windows", True)
        config = self.mod.load_config()
        config["desktop_automation"]["artifact_root"] = str(Path(self.tmpdir.name) / "desktop-artifacts")
        target = config["desktop_automation"]["targets"]["windows"]
        contract = self.mod.desktop_target_contract("windows", target)
        receipt = {
            "target": "windows",
            "adapter": target["adapter"],
            "bootstrap": target["bootstrap"],
            "target_type": target["target_type"],
            "host": target.get("host"),
            "repo_path": target.get("repo_path"),
            "artifact_root": config["desktop_automation"]["artifact_root"],
            "capability_tier": target.get("capability_tier", "v2"),
            "installed_at": self.mod.now_iso(),
            "remote_bootstrap_ready": True,
            "contract": contract,
        }
        self.mod.atomic_write_text(
            self.mod.desktop_target_receipt_path("windows"),
            json.dumps(receipt, indent=2) + "\n",
        )
        remote_manifest = {
            "schema": 1,
            "job_id": "job-123",
            "target": "windows",
            "action": "click",
            "label": "ui-preview",
            "status": "pass",
            "pid": 5153,
            "window": {"title": "UI Preview"},
        }

        def fake_fetch(_host, remote_path, local_path, *, optional=False, timeout=60):
            local_path.parent.mkdir(parents=True, exist_ok=True)
            if local_path.name == "ui-tree.json":
                local_path.write_text(json.dumps({"id": "root", "type": "Window", "children": [{"id": "toggle", "type": "Toggle"}]}))
            elif local_path.suffix == ".log":
                local_path.write_text(f"log:{Path(remote_path).name}\n")
            else:
                local_path.write_bytes(b"png")
            return True

        def fake_image_change(before_path, after_path, *, diff_output_path=None):
            if diff_output_path is not None:
                diff_output_path.parent.mkdir(parents=True, exist_ok=True)
                diff_output_path.write_bytes(b"diff")
            return {"changed": True, "method": "pixel-bbox", "bbox": {"left": 1, "top": 2, "right": 3, "bottom": 4}}

        with mock.patch.object(self.mod, "ensure_host_reachable", return_value="win2"):
            with mock.patch.object(
                self.mod,
                "probe_windows_session_agent",
                return_value={
                    "task_name": contract["task_name"],
                    "task_present": True,
                    "task_state": "Ready",
                    "interactive_user": r"DESKTOP\daniel",
                    "remote_root": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent",
                    "agent_root_exists": True,
                    "jobs_dir": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\jobs",
                    "jobs_dir_exists": True,
                    "results_dir": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\results",
                    "results_dir_exists": True,
                    "script_path": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\agent.ps1",
                    "script_exists": True,
                },
            ):
                with mock.patch.object(self.mod, "windows_ssh_write_text") as write_mock:
                    with mock.patch.object(self.mod, "start_windows_session_agent_task") as start_mock:
                        with mock.patch.object(self.mod, "windows_ssh_read_json", return_value=remote_manifest):
                            with mock.patch.object(self.mod, "windows_ssh_fetch_file", side_effect=fake_fetch) as fetch_mock:
                                with mock.patch.object(self.mod, "windows_ssh_remove_path") as cleanup_mock:
                                    with mock.patch.object(self.mod, "image_change_summary", side_effect=fake_image_change):
                                        manifest = self.mod.run_windows_session_agent_action(
                                            config,
                                            "windows",
                                            target,
                                            r"C:\Pulp\ui-preview.exe",
                                            action_name="click",
                                            label="ui-preview",
                                            output_path=None,
                                            pulp_app_automation=True,
                                            capture_ui_snapshot=True,
                                            click_point=None,
                                            click_view_id="bypass-toggle",
                                            click_view_type=None,
                                            click_view_text=None,
                                            click_view_label=None,
                                            capture_before=True,
                                            settle_secs=0.75,
                                            timeout_secs=5.0,
                                        )

        self.assertEqual(manifest["agent_status"], "pass")
        self.assertEqual(manifest["interaction"]["mode"], "pulp-app")
        self.assertEqual(manifest["inspector"]["view_count"], 2)
        self.assertTrue(manifest["artifacts"]["diff_screenshot"].endswith("/screenshots/diff.png"))
        self.assertTrue(Path(manifest["artifacts"]["agent_manifest"]).exists())
        write_mock.assert_called_once()
        start_mock.assert_called_once()
        self.assertGreaterEqual(fetch_mock.call_count, 4)
        self.assertGreaterEqual(cleanup_mock.call_count, 2)

    def test_run_windows_session_agent_action_supports_generic_window_capture(self):
        self._set_target_enabled("windows", True)
        config = self.mod.load_config()
        config["desktop_automation"]["artifact_root"] = str(Path(self.tmpdir.name) / "desktop-artifacts")
        target = config["desktop_automation"]["targets"]["windows"]
        contract = self.mod.desktop_target_contract("windows", target)
        receipt = {
            "target": "windows",
            "adapter": target["adapter"],
            "bootstrap": target["bootstrap"],
            "target_type": target["target_type"],
            "host": target.get("host"),
            "repo_path": target.get("repo_path"),
            "artifact_root": config["desktop_automation"]["artifact_root"],
            "capability_tier": target.get("capability_tier", "v2"),
            "installed_at": self.mod.now_iso(),
            "remote_bootstrap_ready": True,
            "contract": contract,
        }
        self.mod.atomic_write_text(
            self.mod.desktop_target_receipt_path("windows"),
            json.dumps(receipt, indent=2) + "\n",
        )
        remote_manifest = {
            "schema": 1,
            "job_id": "job-generic",
            "target": "windows",
            "action": "click",
            "label": "calculator",
            "status": "pass",
            "pid": 6161,
            "window": {
                "title": "Calculator",
                "bounds": {"left": 10, "top": 20, "right": 210, "bottom": 260, "width": 200, "height": 240},
            },
            "interaction": {"mode": "window-capture", "click": {"screen_point": {"x": 80, "y": 120}}},
        }

        def fake_fetch(_host, remote_path, local_path, *, optional=False, timeout=60):
            local_path.parent.mkdir(parents=True, exist_ok=True)
            if local_path.suffix == ".log":
                local_path.write_text("log\n")
            else:
                local_path.write_bytes(b"png")
            return True

        def fake_image_change(before_path, after_path, *, diff_output_path=None):
            if diff_output_path is not None:
                diff_output_path.parent.mkdir(parents=True, exist_ok=True)
                diff_output_path.write_bytes(b"diff")
            return {"changed": True, "method": "pixel-bbox", "bbox": {"left": 1, "top": 2, "right": 3, "bottom": 4}}

        with mock.patch.object(self.mod, "ensure_host_reachable", return_value="win"):
            with mock.patch.object(
                self.mod,
                "probe_windows_session_agent",
                return_value={
                    "task_name": contract["task_name"],
                    "task_present": True,
                    "task_state": "Ready",
                    "interactive_user": r"WIN\danielraffel",
                    "remote_root": r"C:\Users\danielraffel\AppData\Local\Pulp\desktop-automation-agent",
                    "agent_root_exists": True,
                    "jobs_dir": r"C:\Users\danielraffel\AppData\Local\Pulp\desktop-automation-agent\jobs",
                    "jobs_dir_exists": True,
                    "results_dir": r"C:\Users\danielraffel\AppData\Local\Pulp\desktop-automation-agent\results",
                    "results_dir_exists": True,
                    "script_path": r"C:\Users\danielraffel\AppData\Local\Pulp\desktop-automation-agent\agent.ps1",
                    "script_exists": True,
                },
            ):
                with mock.patch.object(self.mod, "windows_ssh_write_text"):
                    with mock.patch.object(self.mod, "start_windows_session_agent_task"):
                        with mock.patch.object(self.mod, "windows_ssh_read_json", return_value=remote_manifest):
                            with mock.patch.object(self.mod, "windows_ssh_fetch_file", side_effect=fake_fetch):
                                with mock.patch.object(self.mod, "windows_ssh_remove_path"):
                                    with mock.patch.object(self.mod, "image_change_summary", side_effect=fake_image_change):
                                        manifest = self.mod.run_windows_session_agent_action(
                                            config,
                                            "windows",
                                            target,
                                            "calc.exe",
                                            action_name="click",
                                            label="calculator",
                                            output_path=None,
                                            pulp_app_automation=False,
                                            capture_ui_snapshot=False,
                                            click_point="70,256",
                                            click_view_id=None,
                                            click_view_type=None,
                                            click_view_text=None,
                                            click_view_label=None,
                                            capture_before=True,
                                            settle_secs=0.75,
                                            timeout_secs=5.0,
                                        )

        self.assertEqual(manifest["agent_status"], "pass")
        self.assertEqual(manifest["interaction"]["mode"], "window-capture")
        self.assertEqual(manifest["window"]["title"], "Calculator")
        self.assertNotIn("inspector", manifest)
        self.assertIn("diff_screenshot", manifest["artifacts"])

    def test_run_linux_xvfb_remote_action_exact_sha_attaches_source_and_launch_cwd(self):
        config = self.mod.load_config()
        config["desktop_automation"]["artifact_root"] = str(Path(self.tmpdir.name) / "desktop-artifacts")
        target = config["desktop_automation"]["targets"]["ubuntu"]
        prepared_root = "$HOME/.local/state/pulp/desktop-source/ubuntu/abc123"
        source_context = {
            "mode": "exact-sha",
            "branch": "feature/source",
            "sha": "c" * 40,
            "prepare_command": "./scripts/build-ui-preview.sh",
            "prepare_timeout_secs": 180.0,
            "prepared_root": prepared_root,
            "prepared_root_display": "~/.local/state/pulp/desktop-source/ubuntu/abc123",
            "launch_cwd": prepared_root,
            "launch_cwd_display": "~/.local/state/pulp/desktop-source/ubuntu/abc123",
            "launch_command": "$HOME/.local/state/pulp/desktop-source/ubuntu/abc123/build/ui-preview",
            "prepare_log": str(Path(self.tmpdir.name) / "prepare.log"),
            "prepared_state": "clean",
        }

        def fake_fetch(_host, remote_path, local_path, *, optional=False, timeout=60):
            local_path.parent.mkdir(parents=True, exist_ok=True)
            if local_path.name == "ui-tree.json":
                local_path.write_text(json.dumps({"id": "root", "type": "Window"}))
            elif local_path.suffix == ".log":
                local_path.write_text(f"log:{Path(remote_path).name}\n")
            else:
                local_path.write_bytes(b"png")
            return True

        with mock.patch.object(self.mod, "ensure_host_reachable", return_value="ubuntu"):
            with mock.patch.object(self.mod, "prepare_linux_exact_sha_source", return_value=source_context):
                with mock.patch.object(self.mod, "probe_linux_launch_backend", return_value={"mode": "xvfb", "path": "/usr/bin/xvfb-run"}):
                    with mock.patch.object(self.mod, "build_linux_xvfb_remote_command", return_value="remote-cmd") as build_mock:
                        with mock.patch.object(self.mod.subprocess, "run", return_value=subprocess.CompletedProcess(["ssh"], 0, "", "")):
                            with mock.patch.object(self.mod, "fetch_ssh_artifact", side_effect=fake_fetch):
                                with mock.patch.object(self.mod, "cleanup_remote_ssh_dir"):
                                    manifest = self.mod.run_linux_xvfb_remote_action(
                                        config,
                                        "ubuntu",
                                        target,
                                        str(self.mod.ROOT / "build" / "ui-preview"),
                                        action_name="click",
                                        label="ui-preview",
                                        output_path=None,
                                        pulp_app_automation=True,
                                        capture_ui_snapshot=True,
                                        click_point=None,
                                        click_view_id="bypass-toggle",
                                        click_view_type=None,
                                        click_view_text=None,
                                        click_view_label=None,
                                        capture_before=True,
                                        settle_secs=0.75,
                                        timeout_secs=5.0,
                                        source_request={
                                        "mode": "exact-sha",
                                        "branch": "feature/source",
                                        "sha": "c" * 40,
                                        "prepare_command": "./scripts/build-ui-preview.sh",
                                        "prepare_timeout_secs": 180.0,
                                    },
                                )

        self.assertEqual(build_mock.call_args.kwargs["launch_cwd"], prepared_root)
        self.assertEqual(build_mock.call_args.args[2], source_context["launch_command"])
        self.assertEqual(build_mock.call_args.kwargs["launch_backend"], {"mode": "xvfb", "path": "/usr/bin/xvfb-run"})
        self.assertEqual(manifest["source"]["mode"], "exact-sha")
        self.assertEqual(manifest["source"]["launch_cwd"], "~/.local/state/pulp/desktop-source/ubuntu/abc123")
        self.assertTrue(manifest["artifacts"]["prepare_log"].endswith("prepare.log"))

    def test_run_windows_session_agent_action_exact_sha_attaches_source_and_launch_cwd(self):
        self._set_target_enabled("windows", True)
        config = self.mod.load_config()
        config["desktop_automation"]["artifact_root"] = str(Path(self.tmpdir.name) / "desktop-artifacts")
        target = config["desktop_automation"]["targets"]["windows"]
        contract = self.mod.desktop_target_contract("windows", target)
        receipt = {
            "target": "windows",
            "adapter": target["adapter"],
            "bootstrap": target["bootstrap"],
            "target_type": target["target_type"],
            "host": target.get("host"),
            "repo_path": target.get("repo_path"),
            "artifact_root": config["desktop_automation"]["artifact_root"],
            "capability_tier": target.get("capability_tier", "v2"),
            "installed_at": self.mod.now_iso(),
            "remote_bootstrap_ready": True,
            "contract": contract,
        }
        self.mod.atomic_write_text(
            self.mod.desktop_target_receipt_path("windows"),
            json.dumps(receipt, indent=2) + "\n",
        )
        source_context = {
            "mode": "exact-sha",
            "branch": "feature/source",
            "sha": "d" * 40,
            "prepare_command": ".\\scripts\\build-ui-preview.ps1",
            "prepare_timeout_secs": 240.0,
            "prepared_root": r"$env:LOCALAPPDATA\Pulp\desktop-source\windows\abc123",
            "launch_cwd": r"$env:LOCALAPPDATA\Pulp\desktop-source\windows\abc123",
            "launch_command": r"$env:LOCALAPPDATA\Pulp\desktop-source\windows\abc123\build\ui-preview.exe",
            "prepare_log": str(Path(self.tmpdir.name) / "prepare.log"),
            "prepared_state": "clean",
        }
        remote_manifest = {
            "schema": 1,
            "job_id": "job-456",
            "target": "windows",
            "action": "inspect",
            "label": "ui-preview",
            "status": "pass",
            "pid": 6262,
            "window": {"title": "UI Preview"},
        }

        def fake_fetch(_host, remote_path, local_path, *, optional=False, timeout=60):
            local_path.parent.mkdir(parents=True, exist_ok=True)
            if local_path.suffix == ".log":
                local_path.write_text("log\n")
            elif local_path.name == "ui-tree.json":
                local_path.write_text(json.dumps({"id": "root", "type": "Window"}))
            else:
                local_path.write_bytes(b"png")
            return True

        with mock.patch.object(self.mod, "ensure_host_reachable", return_value="win2"):
            with mock.patch.object(
                self.mod,
                "probe_windows_session_agent",
                return_value={
                    "task_name": contract["task_name"],
                    "task_present": True,
                    "task_state": "Ready",
                    "interactive_user": r"DESKTOP\daniel",
                    "remote_root": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent",
                    "agent_root_exists": True,
                    "jobs_dir": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\jobs",
                    "jobs_dir_exists": True,
                    "results_dir": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\results",
                    "results_dir_exists": True,
                    "script_path": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\agent.ps1",
                    "script_exists": True,
                },
            ):
                with mock.patch.object(self.mod, "prepare_windows_exact_sha_source", return_value=source_context):
                    with mock.patch.object(self.mod, "build_windows_session_agent_request") as request_mock:
                        request_mock.return_value = {
                            "job_id": "job-456",
                            "outputs": {
                                "result_root": r"C:\tmp\result",
                                "manifest": r"C:\tmp\result\manifest.json",
                                "stdout": r"C:\tmp\result\stdout.log",
                                "stderr": r"C:\tmp\result\stderr.log",
                                "screenshot": r"C:\tmp\result\screenshots\window.png",
                                "ui_snapshot": r"C:\tmp\result\ui-tree.json",
                            },
                        }
                        with mock.patch.object(self.mod, "windows_ssh_write_text"):
                            with mock.patch.object(self.mod, "start_windows_session_agent_task"):
                                with mock.patch.object(self.mod, "windows_ssh_read_json", return_value=remote_manifest):
                                    with mock.patch.object(self.mod, "windows_ssh_fetch_file", side_effect=fake_fetch):
                                        with mock.patch.object(self.mod, "windows_ssh_remove_path"):
                                            manifest = self.mod.run_windows_session_agent_action(
                                                config,
                                                "windows",
                                                target,
                                                str(self.mod.ROOT / "build" / "ui-preview.exe"),
                                                action_name="inspect",
                                                label="ui-preview",
                                                output_path=None,
                                                pulp_app_automation=True,
                                                capture_ui_snapshot=True,
                                                click_point=None,
                                                click_view_id=None,
                                                click_view_type=None,
                                                click_view_text=None,
                                                click_view_label=None,
                                                capture_before=False,
                                                settle_secs=0.0,
                                                timeout_secs=5.0,
                                                source_request={
                                                    "mode": "exact-sha",
                                                    "branch": "feature/source",
                                                    "sha": "d" * 40,
                                                    "prepare_command": ".\\scripts\\build-ui-preview.ps1",
                                                    "prepare_timeout_secs": 240.0,
                                                },
                                            )

        self.assertEqual(request_mock.call_args.args[2], source_context["launch_command"])
        self.assertEqual(request_mock.call_args.kwargs["repo_path"], source_context["launch_cwd"])
        self.assertEqual(manifest["source"]["mode"], "exact-sha")
        self.assertEqual(manifest["source"]["sha"], "d" * 40)
        self.assertTrue(manifest["artifacts"]["prepare_log"].endswith("prepare.log"))

    def test_run_windows_session_agent_action_accepts_logged_on_user_without_active_console(self):
        self._set_target_enabled("windows", True)
        config = self.mod.load_config()
        config["desktop_automation"]["artifact_root"] = str(Path(self.tmpdir.name) / "desktop-artifacts")
        target = config["desktop_automation"]["targets"]["windows"]
        contract = self.mod.desktop_target_contract("windows", target)
        receipt = {
            "target": "windows",
            "adapter": target["adapter"],
            "bootstrap": target["bootstrap"],
            "target_type": target["target_type"],
            "host": target.get("host"),
            "repo_path": target.get("repo_path"),
            "artifact_root": config["desktop_automation"]["artifact_root"],
            "capability_tier": target.get("capability_tier", "v2"),
            "installed_at": self.mod.now_iso(),
            "remote_bootstrap_ready": True,
            "contract": contract,
        }
        self.mod.atomic_write_text(
            self.mod.desktop_target_receipt_path("windows"),
            json.dumps(receipt, indent=2) + "\n",
        )
        remote_manifest = {
            "schema": 1,
            "job_id": "job-disc",
            "target": "windows",
            "action": "inspect",
            "label": "ui-preview",
            "status": "pass",
            "pid": 5151,
            "window": {"title": "UI Preview"},
        }

        def fake_fetch(_host, remote_path, local_path, *, optional=False, timeout=60):
            local_path.parent.mkdir(parents=True, exist_ok=True)
            if local_path.name == "ui-tree.json":
                local_path.write_text(json.dumps({"id": "root", "type": "Window"}))
            elif local_path.suffix == ".log":
                local_path.write_text("log\n")
            else:
                local_path.write_bytes(b"png")
            return True

        with mock.patch.object(self.mod, "ensure_host_reachable", return_value="win2"):
            with mock.patch.object(
                self.mod,
                "probe_windows_session_agent",
                return_value={
                    "task_name": contract["task_name"],
                    "task_present": True,
                    "task_state": "Ready",
                    "interactive_user": "",
                    "logged_on_user": "danielraffel",
                    "session_state": "Disc",
                    "remote_root": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent",
                    "agent_root_exists": True,
                    "jobs_dir": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\jobs",
                    "jobs_dir_exists": True,
                    "results_dir": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\results",
                    "results_dir_exists": True,
                    "script_path": r"C:\Users\daniel\AppData\Local\Pulp\desktop-automation-agent\agent.ps1",
                    "script_exists": True,
                },
            ):
                with mock.patch.object(self.mod, "windows_ssh_write_text"):
                    with mock.patch.object(self.mod, "start_windows_session_agent_task"):
                        with mock.patch.object(self.mod, "windows_ssh_read_json", return_value=remote_manifest):
                            with mock.patch.object(self.mod, "windows_ssh_fetch_file", side_effect=fake_fetch):
                                with mock.patch.object(self.mod, "windows_ssh_remove_path"):
                                    manifest = self.mod.run_windows_session_agent_action(
                                        config,
                                        "windows",
                                        target,
                                        r"C:\Pulp\ui-preview.exe",
                                        action_name="inspect",
                                        label="ui-preview",
                                        output_path=None,
                                        pulp_app_automation=True,
                                        capture_ui_snapshot=True,
                                        click_point=None,
                                        click_view_id=None,
                                        click_view_type=None,
                                        click_view_text=None,
                                        click_view_label=None,
                                        capture_before=False,
                                        settle_secs=0.0,
                                        timeout_secs=5.0,
                                    )

        self.assertEqual(manifest["agent_status"], "pass")
        self.assertEqual(manifest["window"]["title"], "UI Preview")

    def test_cmd_desktop_smoke_reports_success(self):
        manifest = {
            "label": "calculator",
            "pid": 4242,
            "artifacts": {
                "screenshot": "/tmp/calc.png",
                "bundle_dir": "/tmp/desktop-run",
            },
        }

        with mock.patch.object(self.mod, "run_macos_local_smoke", return_value=manifest):
            original_platform = self.mod.sys.platform
            self.mod.sys.platform = "darwin"
            try:
                buf = io.StringIO()
                with redirect_stdout(buf):
                    exit_code = self.mod.cmd_desktop_smoke(
                        SimpleNamespace(
                            target="mac",
                            launch_command="/System/Applications/Calculator.app/Contents/MacOS/Calculator",
                            bundle_id=None,
                            label="calculator",
                            output="/tmp/calc.png",
                            capture_ui_snapshot=False,
                            click=None,
                            click_view_id=None,
                            click_view_type=None,
                            click_view_text=None,
                            click_view_label=None,
                            capture_before=False,
                            settle_secs=0.5,
                            timeout=5.0,
                            json=False,
                        )
                    )
            finally:
                self.mod.sys.platform = original_platform

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop smoke PASS for `mac`", output)
        self.assertIn("/tmp/calc.png", output)

    def test_cmd_desktop_smoke_reports_ui_snapshot_success(self):
        manifest = {
            "label": "ui-preview",
            "pid": 4343,
            "artifacts": {
                "screenshot": "/tmp/ui-preview.png",
                "ui_snapshot": "/tmp/ui-tree.json",
                "bundle_dir": "/tmp/desktop-run",
            },
        }

        with mock.patch.object(self.mod, "run_macos_local_smoke", return_value=manifest):
            original_platform = self.mod.sys.platform
            self.mod.sys.platform = "darwin"
            try:
                buf = io.StringIO()
                with redirect_stdout(buf):
                    exit_code = self.mod.cmd_desktop_smoke(
                        SimpleNamespace(
                            target="mac",
                            launch_command="/tmp/pulp-ui-preview",
                            bundle_id=None,
                            label="ui-preview",
                            output="/tmp/ui-preview.png",
                            capture_ui_snapshot=True,
                            click=None,
                            click_view_id=None,
                            click_view_type=None,
                            click_view_text=None,
                            click_view_label=None,
                            capture_before=False,
                            settle_secs=0.5,
                            timeout=5.0,
                            json=False,
                        )
                    )
            finally:
                self.mod.sys.platform = original_platform

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("/tmp/ui-tree.json", output)

    def test_cmd_desktop_smoke_dispatches_linux_xvfb_runner(self):
        manifest = {
            "label": "ui-preview",
            "pid": None,
            "artifacts": {
                "screenshot": "/tmp/ui-preview.png",
                "bundle_dir": "/tmp/desktop-run",
            },
        }

        with mock.patch.object(self.mod, "run_linux_xvfb_remote_action", return_value=manifest) as runner_mock:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_desktop_smoke(
                    SimpleNamespace(
                        target="ubuntu",
                        launch_command="/tmp/pulp-ui-preview",
                        bundle_id=None,
                        label="ui-preview",
                        output="/tmp/ui-preview.png",
                        capture_ui_snapshot=True,
                        click=None,
                        click_view_id=None,
                        click_view_type=None,
                        click_view_text=None,
                        click_view_label=None,
                        pulp_app_automation=True,
                        capture_before=False,
                        settle_secs=0.5,
                        timeout=5.0,
                        json=False,
                    )
                )

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop smoke PASS for `ubuntu`", output)
        self.assertEqual(runner_mock.call_args.kwargs["action_name"], "smoke")
        self.assertEqual(runner_mock.call_args.args[3], "/tmp/pulp-ui-preview")

    def test_cmd_desktop_smoke_linux_allows_generic_x11_runner(self):
        manifest = {
            "label": "ui-preview",
            "pid": None,
            "artifacts": {
                "screenshot": "/tmp/ui-preview.png",
                "bundle_dir": "/tmp/desktop-run",
            },
        }

        with mock.patch.object(self.mod, "run_linux_xvfb_remote_action", return_value=manifest) as runner_mock:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_desktop_smoke(
                    SimpleNamespace(
                        target="ubuntu",
                        launch_command="/tmp/pulp-ui-preview",
                        bundle_id=None,
                        label="ui-preview",
                        output="/tmp/ui-preview.png",
                        capture_ui_snapshot=False,
                        click=None,
                        click_view_id=None,
                        click_view_type=None,
                        click_view_text=None,
                        click_view_label=None,
                        pulp_app_automation=False,
                        capture_before=False,
                        settle_secs=0.5,
                        timeout=5.0,
                        json=False,
                    )
                )

        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop smoke PASS for `ubuntu`", buf.getvalue())
        self.assertFalse(runner_mock.call_args.kwargs["pulp_app_automation"])
        self.assertFalse(runner_mock.call_args.kwargs["capture_ui_snapshot"])

    def test_cmd_desktop_smoke_dispatches_windows_session_runner(self):
        self._set_target_enabled("windows", True)
        manifest = {
            "label": "ui-preview",
            "pid": 5150,
            "artifacts": {
                "screenshot": "/tmp/ui-preview.png",
                "bundle_dir": "/tmp/desktop-run",
            },
        }

        with mock.patch.object(self.mod, "run_windows_session_agent_action", return_value=manifest) as runner_mock:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_desktop_smoke(
                    SimpleNamespace(
                        target="windows",
                        launch_command=r"C:\Pulp\ui-preview.exe",
                        bundle_id=None,
                        label="ui-preview",
                        output="/tmp/ui-preview.png",
                        capture_ui_snapshot=True,
                        click=None,
                        click_view_id=None,
                        click_view_type=None,
                        click_view_text=None,
                        click_view_label=None,
                        pulp_app_automation=True,
                        capture_before=False,
                        settle_secs=0.5,
                        timeout=5.0,
                        json=False,
                    )
                )

        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop smoke PASS for `windows`", buf.getvalue())
        self.assertEqual(runner_mock.call_args.kwargs["action_name"], "smoke")
        self.assertTrue(runner_mock.call_args.kwargs["pulp_app_automation"])

    def test_cmd_desktop_smoke_windows_allows_generic_window_capture(self):
        self._set_target_enabled("windows", True)
        manifest = {
            "label": "calculator",
            "pid": 5150,
            "artifacts": {
                "screenshot": "/tmp/calc.png",
                "bundle_dir": "/tmp/desktop-run",
            },
        }

        with mock.patch.object(self.mod, "run_windows_session_agent_action", return_value=manifest) as runner_mock:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_desktop_smoke(
                    SimpleNamespace(
                        target="windows",
                        launch_command="calc.exe",
                        bundle_id=None,
                        label="calculator",
                        output="/tmp/calc.png",
                        capture_ui_snapshot=False,
                        click=None,
                        click_view_id=None,
                        click_view_type=None,
                        click_view_text=None,
                        click_view_label=None,
                        pulp_app_automation=False,
                        capture_before=False,
                        settle_secs=0.5,
                        timeout=5.0,
                        json=False,
                    )
                )

        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop smoke PASS for `windows`", buf.getvalue())
        self.assertFalse(runner_mock.call_args.kwargs["pulp_app_automation"])

    def test_cmd_desktop_smoke_windows_generic_rejects_ui_snapshot(self):
        self._set_target_enabled("windows", True)
        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_desktop_smoke(
                SimpleNamespace(
                    target="windows",
                    launch_command="calc.exe",
                    bundle_id=None,
                    label="calculator",
                    output="/tmp/calc.png",
                    capture_ui_snapshot=True,
                    click=None,
                    click_view_id=None,
                    click_view_type=None,
                    click_view_text=None,
                    click_view_label=None,
                    pulp_app_automation=False,
                    capture_before=False,
                    settle_secs=0.5,
                    timeout=5.0,
                    json=False,
                )
            )

        self.assertEqual(exit_code, 1)
        self.assertIn("supports --capture-ui-snapshot only with --pulp-app-automation", buf.getvalue())

    def test_cmd_desktop_smoke_json_emits_manifest(self):
        manifest = {
            "label": "calculator",
            "pid": 4242,
            "artifacts": {
                "screenshot": "/tmp/calc.png",
                "bundle_dir": "/tmp/desktop-run",
            },
        }

        with mock.patch.object(self.mod, "run_macos_local_smoke", return_value=manifest):
            original_platform = self.mod.sys.platform
            self.mod.sys.platform = "darwin"
            try:
                buf = io.StringIO()
                with redirect_stdout(buf):
                    exit_code = self.mod.cmd_desktop_smoke(
                        SimpleNamespace(
                            target="mac",
                            launch_command="/tmp/fake-binary",
                            bundle_id=None,
                            label="calculator",
                            output="/tmp/calc.png",
                            capture_ui_snapshot=False,
                            click=None,
                            click_view_id=None,
                            click_view_type=None,
                            click_view_text=None,
                            click_view_label=None,
                            capture_before=False,
                            settle_secs=0.5,
                            timeout=5.0,
                            json=True,
                        )
                    )
            finally:
                self.mod.sys.platform = original_platform

        payload = json.loads(buf.getvalue())
        self.assertEqual(exit_code, 0)
        self.assertEqual(payload["label"], "calculator")
        self.assertEqual(payload["artifacts"]["bundle_dir"], "/tmp/desktop-run")

    def test_cmd_desktop_click_reports_success(self):
        manifest = {
            "label": "ui-preview",
            "pid": 4242,
            "artifacts": {
                "before_screenshot": "/tmp/before.png",
                "diff_screenshot": "/tmp/diff.png",
                "image_change": {"changed": True, "method": "pixel-bbox", "bbox": {"left": 1, "top": 2, "right": 3, "bottom": 4}},
                "screenshot": "/tmp/after.png",
                "ui_snapshot": "/tmp/ui-tree.json",
                "bundle_dir": "/tmp/desktop-run",
            },
            "interaction": {
                "mode": "pulp-app",
                "click": {"screen_point": {"x": 120, "y": 260}},
            },
        }

        with mock.patch.object(self.mod, "run_macos_local_smoke", return_value=manifest) as click_mock:
            original_platform = self.mod.sys.platform
            self.mod.sys.platform = "darwin"
            try:
                buf = io.StringIO()
                with redirect_stdout(buf):
                    exit_code = self.mod.cmd_desktop_click(
                        SimpleNamespace(
                            target="mac",
                            launch_command="/tmp/pulp-ui-preview",
                            bundle_id=None,
                            label="ui-preview",
                            output="/tmp/after.png",
                            capture_ui_snapshot=True,
                            click=None,
                            click_view_id="bypass-toggle",
                            click_view_type=None,
                            click_view_text=None,
                            click_view_label=None,
                            pulp_app_automation=True,
                            settle_secs=0.5,
                            timeout=5.0,
                            json=False,
                        )
                    )
            finally:
                self.mod.sys.platform = original_platform

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop click PASS for `mac`", output)
        self.assertIn("/tmp/diff.png", output)
        self.assertIn("interaction_mode: pulp-app", output)
        self.assertEqual(click_mock.call_args.kwargs["action_name"], "click")
        self.assertEqual(click_mock.call_args.kwargs["capture_before"], True)

    def test_cmd_desktop_click_dispatches_linux_xvfb_runner(self):
        manifest = {
            "label": "ui-preview",
            "pid": None,
            "artifacts": {
                "before_screenshot": "/tmp/before.png",
                "screenshot": "/tmp/after.png",
                "bundle_dir": "/tmp/desktop-run",
            },
            "interaction": {"mode": "pulp-app"},
        }

        with mock.patch.object(self.mod, "run_linux_xvfb_remote_action", return_value=manifest) as runner_mock:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_desktop_click(
                    SimpleNamespace(
                        target="ubuntu",
                        launch_command="/tmp/pulp-ui-preview",
                        bundle_id=None,
                        label="ui-preview",
                        output="/tmp/after.png",
                        capture_ui_snapshot=True,
                        click=None,
                        click_view_id="bypass-toggle",
                        click_view_type=None,
                        click_view_text=None,
                        click_view_label=None,
                        pulp_app_automation=True,
                        settle_secs=0.5,
                        timeout=5.0,
                        json=False,
                    )
                )

        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop click PASS for `ubuntu`", buf.getvalue())
        self.assertEqual(runner_mock.call_args.kwargs["action_name"], "click")
        self.assertTrue(runner_mock.call_args.kwargs["capture_before"])

    def test_cmd_desktop_click_requires_click_selector(self):
        original_platform = self.mod.sys.platform
        self.mod.sys.platform = "darwin"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_desktop_click(
                    SimpleNamespace(
                        target="mac",
                        launch_command="/tmp/pulp-ui-preview",
                        bundle_id=None,
                        label="ui-preview",
                        output="/tmp/after.png",
                        capture_ui_snapshot=False,
                        click=None,
                        click_view_id=None,
                        click_view_type=None,
                        click_view_text=None,
                        click_view_label=None,
                        pulp_app_automation=False,
                        settle_secs=0.5,
                        timeout=5.0,
                        json=False,
                    )
                )
        finally:
            self.mod.sys.platform = original_platform

        output = buf.getvalue()
        self.assertEqual(exit_code, 1)
        self.assertIn("requires --click or one view-target selector", output)

    def test_cmd_desktop_click_dispatches_windows_session_runner(self):
        self._set_target_enabled("windows", True)
        manifest = {
            "label": "ui-preview",
            "pid": 5151,
            "artifacts": {
                "before_screenshot": "/tmp/before.png",
                "screenshot": "/tmp/after.png",
                "bundle_dir": "/tmp/desktop-run",
            },
            "interaction": {"mode": "pulp-app"},
        }

        with mock.patch.object(self.mod, "run_windows_session_agent_action", return_value=manifest) as runner_mock:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_desktop_click(
                    SimpleNamespace(
                        target="windows",
                        launch_command=r"C:\Pulp\ui-preview.exe",
                        bundle_id=None,
                        label="ui-preview",
                        output="/tmp/after.png",
                        capture_ui_snapshot=True,
                        click=None,
                        click_view_id="bypass-toggle",
                        click_view_type=None,
                        click_view_text=None,
                        click_view_label=None,
                        pulp_app_automation=True,
                        settle_secs=0.5,
                        timeout=5.0,
                        json=False,
                    )
                )

        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop click PASS for `windows`", buf.getvalue())
        self.assertEqual(runner_mock.call_args.kwargs["action_name"], "click")
        self.assertTrue(runner_mock.call_args.kwargs["capture_before"])

    def test_cmd_desktop_click_windows_allows_generic_window_capture_point_click(self):
        self._set_target_enabled("windows", True)
        manifest = {
            "label": "calculator",
            "pid": 5151,
            "artifacts": {
                "before_screenshot": "/tmp/before.png",
                "screenshot": "/tmp/after.png",
                "bundle_dir": "/tmp/desktop-run",
            },
            "interaction": {"mode": "window-capture", "click": {"screen_point": {"x": 70, "y": 256}}},
        }

        with mock.patch.object(self.mod, "run_windows_session_agent_action", return_value=manifest) as runner_mock:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_desktop_click(
                    SimpleNamespace(
                        target="windows",
                        launch_command="calc.exe",
                        bundle_id=None,
                        label="calculator",
                        output="/tmp/after.png",
                        capture_ui_snapshot=False,
                        click="70,256",
                        click_view_id=None,
                        click_view_type=None,
                        click_view_text=None,
                        click_view_label=None,
                        pulp_app_automation=False,
                        settle_secs=0.5,
                        timeout=5.0,
                        json=False,
                    )
                )

        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop click PASS for `windows`", buf.getvalue())
        self.assertFalse(runner_mock.call_args.kwargs["pulp_app_automation"])

    def test_cmd_desktop_click_windows_generic_rejects_view_selector(self):
        self._set_target_enabled("windows", True)
        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_desktop_click(
                SimpleNamespace(
                    target="windows",
                    launch_command="calc.exe",
                    bundle_id=None,
                    label="calculator",
                    output="/tmp/after.png",
                    capture_ui_snapshot=False,
                    click=None,
                    click_view_id="bypass-toggle",
                    click_view_type=None,
                    click_view_text=None,
                    click_view_label=None,
                    pulp_app_automation=False,
                    settle_secs=0.5,
                    timeout=5.0,
                    json=False,
                )
            )

        self.assertEqual(exit_code, 1)
        self.assertIn("supports view-target selectors only with --pulp-app-automation", buf.getvalue())

    def test_cmd_desktop_inspect_dispatches_linux_xvfb_runner(self):
        manifest = {
            "label": "ui-preview",
            "pid": None,
            "artifacts": {
                "screenshot": "/tmp/ui-preview.png",
                "ui_snapshot": "/tmp/ui-tree.json",
                "bundle_dir": "/tmp/desktop-run",
            },
        }

        with mock.patch.object(self.mod, "run_linux_xvfb_remote_action", return_value=manifest) as runner_mock:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_desktop_inspect(
                    SimpleNamespace(
                        target="ubuntu",
                        launch_command="/tmp/pulp-ui-preview",
                        bundle_id=None,
                        label="ui-preview",
                        output="/tmp/ui-preview.png",
                        pulp_app_automation=True,
                        timeout=5.0,
                        json=False,
                    )
                )

        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop inspect PASS for `ubuntu`", buf.getvalue())
        self.assertEqual(runner_mock.call_args.kwargs["action_name"], "inspect")
        self.assertTrue(runner_mock.call_args.kwargs["capture_ui_snapshot"])

    def test_cmd_desktop_inspect_linux_allows_generic_x11_runner_without_ui_snapshot(self):
        manifest = {
            "label": "ui-preview",
            "pid": None,
            "artifacts": {
                "screenshot": "/tmp/ui-preview.png",
                "bundle_dir": "/tmp/desktop-run",
            },
        }

        with mock.patch.object(self.mod, "run_linux_xvfb_remote_action", return_value=manifest) as runner_mock:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_desktop_inspect(
                    SimpleNamespace(
                        target="ubuntu",
                        launch_command="/tmp/pulp-ui-preview",
                        bundle_id=None,
                        label="ui-preview",
                        output="/tmp/ui-preview.png",
                        pulp_app_automation=False,
                        timeout=5.0,
                        json=False,
                    )
                )

        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop inspect PASS for `ubuntu`", buf.getvalue())
        self.assertFalse(runner_mock.call_args.kwargs["pulp_app_automation"])
        self.assertFalse(runner_mock.call_args.kwargs["capture_ui_snapshot"])

    def test_cmd_desktop_inspect_dispatches_windows_session_runner(self):
        self._set_target_enabled("windows", True)
        manifest = {
            "label": "ui-preview",
            "pid": 5152,
            "artifacts": {
                "screenshot": "/tmp/ui-preview.png",
                "ui_snapshot": "/tmp/ui-tree.json",
                "bundle_dir": "/tmp/desktop-run",
            },
        }

        with mock.patch.object(self.mod, "run_windows_session_agent_action", return_value=manifest) as runner_mock:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_desktop_inspect(
                    SimpleNamespace(
                        target="windows",
                        launch_command=r"C:\Pulp\ui-preview.exe",
                        bundle_id=None,
                        label="ui-preview",
                        output="/tmp/ui-preview.png",
                        pulp_app_automation=True,
                        timeout=5.0,
                        json=False,
                    )
                )

        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop inspect PASS for `windows`", buf.getvalue())
        self.assertEqual(runner_mock.call_args.kwargs["action_name"], "inspect")
        self.assertTrue(runner_mock.call_args.kwargs["capture_ui_snapshot"])

    def test_cmd_desktop_inspect_windows_allows_generic_window_capture(self):
        self._set_target_enabled("windows", True)
        manifest = {
            "label": "calculator",
            "pid": 5152,
            "artifacts": {
                "screenshot": "/tmp/calc.png",
                "bundle_dir": "/tmp/desktop-run",
            },
        }

        with mock.patch.object(self.mod, "run_windows_session_agent_action", return_value=manifest) as runner_mock:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_desktop_inspect(
                    SimpleNamespace(
                        target="windows",
                        launch_command="calc.exe",
                        bundle_id=None,
                        label="calculator",
                        output="/tmp/calc.png",
                        pulp_app_automation=False,
                        timeout=5.0,
                        json=False,
                    )
                )

        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop inspect PASS for `windows`", buf.getvalue())
        self.assertFalse(runner_mock.call_args.kwargs["pulp_app_automation"])
        self.assertFalse(runner_mock.call_args.kwargs["capture_ui_snapshot"])

    def test_cmd_desktop_inspect_reports_success(self):
        manifest = {
            "label": "ui-preview",
            "pid": 4343,
            "artifacts": {
                "screenshot": "/tmp/ui-preview.png",
                "ui_snapshot": "/tmp/ui-tree.json",
                "bundle_dir": "/tmp/desktop-run",
            },
        }

        with mock.patch.object(self.mod, "run_macos_local_smoke", return_value=manifest) as inspect_mock:
            original_platform = self.mod.sys.platform
            self.mod.sys.platform = "darwin"
            try:
                buf = io.StringIO()
                with redirect_stdout(buf):
                    exit_code = self.mod.cmd_desktop_inspect(
                        SimpleNamespace(
                            target="mac",
                            launch_command="/tmp/pulp-ui-preview",
                            bundle_id=None,
                            label="ui-preview",
                            output="/tmp/ui-preview.png",
                            timeout=5.0,
                            json=False,
                        )
                    )
            finally:
                self.mod.sys.platform = original_platform

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop inspect PASS for `mac`", output)
        self.assertIn("/tmp/ui-tree.json", output)
        self.assertEqual(inspect_mock.call_args.kwargs["action_name"], "inspect")
        self.assertEqual(inspect_mock.call_args.kwargs["capture_ui_snapshot"], True)

    def test_cmd_desktop_inspect_supports_bundle_id_without_ui_snapshot(self):
        manifest = {
            "label": "calculator",
            "pid": 5151,
            "artifacts": {
                "screenshot": "/tmp/calc.png",
                "bundle_dir": "/tmp/desktop-run",
            },
        }

        with mock.patch.object(self.mod, "run_macos_local_smoke", return_value=manifest) as inspect_mock:
            original_platform = self.mod.sys.platform
            self.mod.sys.platform = "darwin"
            try:
                buf = io.StringIO()
                with redirect_stdout(buf):
                    exit_code = self.mod.cmd_desktop_inspect(
                        SimpleNamespace(
                            target="mac",
                            launch_command=None,
                            bundle_id="com.apple.calculator",
                            label="calculator",
                            output="/tmp/calc.png",
                            timeout=5.0,
                            json=False,
                        )
                    )
            finally:
                self.mod.sys.platform = original_platform

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Desktop inspect PASS for `mac`", output)
        self.assertNotIn("ui_snapshot:", output)
        self.assertEqual(inspect_mock.call_args.kwargs["bundle_id"], "com.apple.calculator")
        self.assertEqual(inspect_mock.call_args.kwargs["capture_ui_snapshot"], False)

    def test_cmd_desktop_inspect_requires_exactly_one_launch_mode(self):
        original_platform = self.mod.sys.platform
        self.mod.sys.platform = "darwin"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_desktop_inspect(
                    SimpleNamespace(
                        target="mac",
                        launch_command=None,
                        bundle_id=None,
                        label="calculator",
                        output="/tmp/calc.png",
                        timeout=5.0,
                        json=False,
                    )
                )
        finally:
            self.mod.sys.platform = original_platform

        output = buf.getvalue()
        self.assertEqual(exit_code, 1)
        self.assertIn("requires exactly one of --command or --bundle-id", output)

    def test_run_macos_local_smoke_supports_bundle_id(self):
        config = self.mod.load_config()
        config["desktop_automation"]["artifact_root"] = str(Path(self.tmpdir.name) / "desktop-artifacts")
        screenshot_path = Path(self.tmpdir.name) / "bundle.png"

        with mock.patch.object(self.mod.subprocess, "run") as run_mock:
            with mock.patch.object(
                self.mod,
                "wait_for_macos_bundle_window",
                return_value=(5151, {"windowId": 88, "title": "Calculator", "bounds": {"x": 0, "y": 0, "width": 100, "height": 100}}),
            ):
                with mock.patch.object(self.mod, "capture_macos_window") as capture:
                    manifest = self.mod.run_macos_local_smoke(
                        config,
                        None,
                        action_name="smoke",
                        bundle_id="com.apple.calculator",
                        label="calculator",
                        output_path=str(screenshot_path),
                        capture_ui_snapshot=False,
                        click_point=None,
                        click_view_id=None,
                        click_view_type=None,
                        click_view_text=None,
                        click_view_label=None,
                        capture_before=False,
                        settle_secs=0.5,
                        timeout_secs=1.0,
                    )

        run_mock.assert_any_call(["open", "-b", "com.apple.calculator"], capture_output=True, text=True, check=True)
        capture.assert_called_once_with(88, screenshot_path)
        self.assertEqual(manifest["pid"], 5151)
        self.assertEqual(manifest["bundle_id"], "com.apple.calculator")

    def test_run_macos_local_smoke_refreshes_bundle_window_after_stale_capture(self):
        config = self.mod.load_config()
        config["desktop_automation"]["artifact_root"] = str(Path(self.tmpdir.name) / "desktop-artifacts")
        screenshot_path = Path(self.tmpdir.name) / "bundle.png"

        wait_results = [
            (5151, {"windowId": 88, "title": "Calculator", "bounds": {"x": 0, "y": 0, "width": 100, "height": 100}}),
            (5151, {"windowId": 89, "title": "Calculator", "bounds": {"x": 0, "y": 0, "width": 100, "height": 100}}),
        ]

        with mock.patch.object(self.mod.subprocess, "run") as run_mock:
            with mock.patch.object(self.mod, "wait_for_macos_bundle_window", side_effect=wait_results) as wait_mock:
                with mock.patch.object(
                    self.mod,
                    "capture_macos_window",
                    side_effect=[RuntimeError("stale window"), None],
                ) as capture:
                    manifest = self.mod.run_macos_local_smoke(
                        config,
                        None,
                        action_name="inspect",
                        bundle_id="com.apple.calculator",
                        label="calculator",
                        output_path=str(screenshot_path),
                        capture_ui_snapshot=False,
                        click_point=None,
                        click_view_id=None,
                        click_view_type=None,
                        click_view_text=None,
                        click_view_label=None,
                        capture_before=False,
                        settle_secs=0.5,
                        timeout_secs=1.0,
                    )

        run_mock.assert_any_call(["open", "-b", "com.apple.calculator"], capture_output=True, text=True, check=True)
        self.assertEqual(wait_mock.call_count, 2)
        capture.assert_has_calls([mock.call(88, screenshot_path), mock.call(89, screenshot_path)])
        self.assertEqual(manifest["pid"], 5151)
        self.assertEqual(manifest["window"]["windowId"], 89)

    def test_wait_for_macos_bundle_window_activates_bundle_until_window_is_visible(self):
        payloads = iter(
            [
                {"pid": 5151, "windows": []},
                {"pid": 5151, "windows": [{"windowId": 88, "title": "Calculator"}]},
            ]
        )

        with mock.patch.object(self.mod, "macos_window_info_for_bundle_id", side_effect=lambda bundle_id: next(payloads)):
            with mock.patch.object(self.mod, "activate_macos_bundle_id", return_value={"activated": True}) as activate_mock:
                with mock.patch.object(self.mod.time, "sleep", return_value=None):
                    pid, window = self.mod.wait_for_macos_bundle_window("com.apple.calculator", timeout_secs=1.0)

        self.assertEqual(pid, 5151)
        self.assertEqual(window["windowId"], 88)
        activate_mock.assert_called_once_with("com.apple.calculator")

    def test_capture_macos_window_retries_transient_failures(self):
        output_path = Path(self.tmpdir.name) / "capture.png"
        attempts = {"count": 0}

        def fake_run(cmd, capture_output, text):
            attempts["count"] += 1
            if attempts["count"] == 1:
                return SimpleNamespace(returncode=1, stdout="", stderr="window not ready")
            output_path.parent.mkdir(parents=True, exist_ok=True)
            output_path.write_bytes(b"png")
            return SimpleNamespace(returncode=0, stdout="", stderr="")

        with mock.patch.object(self.mod.subprocess, "run", side_effect=fake_run):
            with mock.patch.object(self.mod.time, "sleep", return_value=None):
                self.mod.capture_macos_window(88, output_path)

        self.assertEqual(attempts["count"], 2)
        self.assertTrue(output_path.exists())

    def test_run_macos_local_smoke_rejects_view_click_without_snapshot(self):
        config = self.mod.load_config()
        config["desktop_automation"]["artifact_root"] = str(Path(self.tmpdir.name) / "desktop-artifacts")

        with self.assertRaises(RuntimeError):
            self.mod.run_macos_local_smoke(
                config,
                "/tmp/fake-binary",
                action_name="smoke",
                bundle_id=None,
                label="ui-preview",
                output_path=str(Path(self.tmpdir.name) / "capture.png"),
                capture_ui_snapshot=False,
                click_point=None,
                click_view_id=None,
                click_view_type="Toggle",
                click_view_text=None,
                click_view_label=None,
                capture_before=False,
                settle_secs=0.5,
                timeout_secs=1.0,
            )

    def test_run_macos_local_smoke_clicks_selected_view_and_captures_before(self):
        config = self.mod.load_config()
        config["desktop_automation"]["artifact_root"] = str(Path(self.tmpdir.name) / "desktop-artifacts")

        class FakeProc:
            def __init__(self):
                self.pid = 5252
                self._returncode = None

            def poll(self):
                return self._returncode

            def terminate(self):
                self._returncode = 0

            def wait(self, timeout=None):
                self._returncode = 0
                return 0

            def kill(self):
                self._returncode = -9

        fake_proc = FakeProc()
        screenshot_path = Path(self.tmpdir.name) / "after.png"
        window = {"windowId": 91, "title": "Preview", "bounds": {"x": 100, "y": 370, "width": 360, "height": 512}}
        view_tree = {
            "type": "View",
            "bounds": {"x": 0, "y": 0, "width": 360, "height": 480},
            "children": [
                {"id": "bypass-toggle", "type": "Toggle", "bounds": {"x": 16, "y": 115, "width": 60, "height": 20}, "visible": True, "on": False}
            ],
        }

        def fake_wait_for_path(path, timeout_secs):
            path.write_text(json.dumps(view_tree))
            return path

        def fake_capture(window_id, output_path):
            path = Path(output_path)
            path.parent.mkdir(parents=True, exist_ok=True)
            payload = b"before" if path.name == "before.png" else b"after"
            path.write_bytes(payload)

        with mock.patch.object(self.mod, "macos_accessibility_trusted", return_value=True):
            with mock.patch.object(self.mod.subprocess, "Popen", return_value=fake_proc):
                with mock.patch.object(self.mod, "wait_for_macos_window", return_value=window):
                    with mock.patch.object(self.mod, "wait_for_path", side_effect=fake_wait_for_path):
                        with mock.patch.object(self.mod, "activate_macos_pid", return_value={"activated": True}):
                                with mock.patch.object(self.mod, "dispatch_macos_click", return_value={"ok": True}) as click_mock:
                                    with mock.patch.object(self.mod, "capture_macos_window", side_effect=fake_capture) as capture:
                                        def fake_image_change(before_path, after_path, *, diff_output_path=None):
                                            if diff_output_path is not None:
                                                diff_output_path.parent.mkdir(parents=True, exist_ok=True)
                                                diff_output_path.write_bytes(b"diff")
                                            return {"changed": True, "method": "pixel-bbox", "bbox": {"left": 1, "top": 2, "right": 3, "bottom": 4}}

                                        with mock.patch.object(
                                            self.mod,
                                            "image_change_summary",
                                            side_effect=fake_image_change,
                                        ):
                                            manifest = self.mod.run_macos_local_smoke(
                                                config,
                                                "/tmp/fake-binary --flag",
                                                action_name="smoke",
                                                bundle_id=None,
                                                label="ui-preview",
                                                output_path=str(screenshot_path),
                                                capture_ui_snapshot=True,
                                                click_point=None,
                                                click_view_id="bypass-toggle",
                                                click_view_type=None,
                                                click_view_text=None,
                                                click_view_label=None,
                                                capture_before=True,
                                                settle_secs=0.0,
                                                timeout_secs=1.0,
                                            )

        before_path = Path(manifest["artifacts"]["before_screenshot"])
        capture.assert_has_calls([
            mock.call(91, before_path),
            mock.call(91, screenshot_path),
        ])
        click_mock.assert_called_once_with(146.0, 527.0)
        self.assertEqual(manifest["artifacts"]["image_change"]["changed"], True)
        self.assertTrue(manifest["artifacts"]["diff_screenshot"].endswith("/screenshots/diff.png"))
        self.assertEqual(manifest["interaction"]["click"]["content_point"], {"x": 46.0, "y": 125.0})
        self.assertEqual(manifest["interaction"]["click"]["selector"]["id"], "bypass-toggle")
        self.assertEqual(manifest["artifacts"]["ui_snapshot"].endswith("ui-tree.json"), True)

    def test_cmd_desktop_smoke_reports_before_screenshot_and_click_point(self):
        manifest = {
            "label": "ui-preview",
            "pid": 5252,
            "artifacts": {
                "before_screenshot": "/tmp/before.png",
                "diff_screenshot": "/tmp/diff.png",
                "image_change": {"changed": True, "method": "pixel-bbox", "bbox": {"left": 1, "top": 2, "right": 3, "bottom": 4}},
                "screenshot": "/tmp/after.png",
                "bundle_dir": "/tmp/desktop-run",
            },
            "interaction": {
                "click": {
                    "screen_point": {"x": 146.0, "y": 527.0},
                }
            },
        }

        with mock.patch.object(self.mod, "run_macos_local_smoke", return_value=manifest):
            original_platform = self.mod.sys.platform
            self.mod.sys.platform = "darwin"
            try:
                buf = io.StringIO()
                with redirect_stdout(buf):
                    exit_code = self.mod.cmd_desktop_smoke(
                        SimpleNamespace(
                            target="mac",
                            launch_command="/tmp/fake-binary",
                            bundle_id=None,
                            label="ui-preview",
                            output="/tmp/after.png",
                            capture_ui_snapshot=False,
                            click="10,20",
                            click_view_id=None,
                            click_view_type=None,
                            click_view_text=None,
                            click_view_label=None,
                            capture_before=True,
                            settle_secs=0.5,
                            timeout=5.0,
                            json=False,
                        )
                    )
            finally:
                self.mod.sys.platform = original_platform

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("/tmp/before.png", output)
        self.assertIn("/tmp/diff.png", output)
        self.assertIn("changed=True method=pixel-bbox", output)
        self.assertIn("1,2 -> 3,4", output)
        self.assertIn("146.0,527.0", output)

    def test_stale_running_job_requeues_when_runner_dies(self):
        job = self.mod.make_job("feature/stale", "3" * 40, "normal", ["mac"], "run", "full")
        job["status"] = "running"
        job["started_at"] = "2026-03-31T00:00:00+00:00"
        job["runner"] = {"pid": 999999, "root": "/tmp/pulp"}
        job["active_targets"] = {
            "mac": {"status": "pass", "duration_secs": 10.0},
            "windows": {"status": "running"},
        }

        self.mod.ensure_state_dirs()
        self.mod.queue_path().write_text(json.dumps([job], indent=2) + "\n")
        self.mod.runner_info_path().write_text(
            json.dumps({"pid": 999999, "active_job_id": job["id"], "active_branch": job["branch"]}) + "\n"
        )

        queue = self.mod.load_queue()
        self.assertEqual(queue[0]["status"], "pending")
        self.assertIn("requeued_at", queue[0])
        self.assertEqual(queue[0]["active_targets"]["mac"]["status"], "pass")
        self.assertEqual(queue[0]["active_targets"]["windows"]["status"], "running")
        self.assertFalse(self.mod.runner_info_path().exists())

    def test_make_job_rejects_unsupported_branch_characters(self):
        with self.assertRaises(ValueError):
            self.mod.make_job("feature/$oops", "3" * 40, "normal", ["mac"], "run", "full")

    def test_stale_running_job_is_superseded_when_newer_pending_exists(self):
        older_job = self.mod.make_job("feature/stale", "3" * 40, "normal", ["mac"], "run", "full")
        older_job["queued_at"] = "2026-03-31T00:00:00+00:00"
        older_job["status"] = "running"
        older_job["started_at"] = "2026-03-31T00:10:00+00:00"
        older_job["runner"] = {"pid": 999999, "root": "/tmp/pulp"}

        newer_job = self.mod.make_job("feature/stale", "4" * 40, "high", ["mac"], "run", "full")
        newer_job["queued_at"] = "2026-03-31T00:20:00+00:00"

        self.mod.ensure_state_dirs()
        self.mod.queue_path().write_text(json.dumps([older_job, newer_job], indent=2) + "\n")
        self.mod.runner_info_path().write_text(
            json.dumps({"pid": 999999, "active_job_id": older_job["id"], "active_branch": older_job["branch"]}) + "\n"
        )

        queue = self.mod.load_queue()
        older_stored = next(job for job in queue if job["id"] == older_job["id"])
        newer_stored = next(job for job in queue if job["id"] == newer_job["id"])

        self.assertEqual(older_stored["status"], "completed")
        self.assertEqual(older_stored["overall"], "superseded")
        self.assertEqual(older_stored["superseded_by"], newer_job["id"])
        self.assertEqual(newer_stored["status"], "pending")

    def test_stale_running_broader_job_is_superseded_by_narrower_same_sha_scope(self):
        broader_job = self.mod.make_job("feature/stale", "3" * 40, "normal", ["mac", "windows"], "run", "smoke")
        broader_job["queued_at"] = "2026-03-31T00:00:00+00:00"
        broader_job["status"] = "running"
        broader_job["started_at"] = "2026-03-31T00:10:00+00:00"
        broader_job["runner"] = {"pid": 999999, "root": "/tmp/pulp"}

        narrower_job = self.mod.make_job("feature/stale", "3" * 40, "normal", ["windows"], "run", "smoke")
        narrower_job["queued_at"] = "2026-03-31T00:20:00+00:00"

        self.mod.ensure_state_dirs()
        self.mod.queue_path().write_text(json.dumps([broader_job, narrower_job], indent=2) + "\n")
        self.mod.runner_info_path().write_text(
            json.dumps({"pid": 999999, "active_job_id": broader_job["id"], "active_branch": broader_job["branch"]}) + "\n"
        )

        queue = self.mod.load_queue()
        broader_stored = next(job for job in queue if job["id"] == broader_job["id"])
        narrower_stored = next(job for job in queue if job["id"] == narrower_job["id"])

        self.assertEqual(broader_stored["status"], "completed")
        self.assertEqual(broader_stored["overall"], "superseded")
        self.assertEqual(broader_stored["superseded_by"], narrower_job["id"])
        self.assertEqual(broader_stored["superseded_reason"], "narrower_scope_queued")
        self.assertEqual(narrower_stored["status"], "pending")

    def test_summarize_active_targets_uses_requested_order(self):
        summary = self.mod.summarize_active_targets(
            {
                "ubuntu": {"status": "running"},
                "windows": {"status": "pending"},
                "mac": {"status": "pass"},
            },
            ["mac", "ubuntu"],
        )
        self.assertEqual(summary, "mac=pass, ubuntu=running, windows=pending")

    def test_update_runner_active_targets_tracks_live_state(self):
        self.mod.write_runner_info(
            {
                "pid": os.getpid(),
                "root": "/tmp/pulp",
                "active_job_id": "job123",
                "active_branch": "feature/live-state",
            }
        )

        self.mod.update_runner_active_targets(
            "job123",
            {
                "mac": {"status": "pass", "duration_secs": 12.3},
                "windows": {"status": "running"},
            },
        )

        info = self.mod.read_runner_info()
        self.assertEqual(info["active_targets"]["mac"]["status"], "pass")
        self.assertEqual(info["active_targets"]["windows"]["status"], "running")
        self.assertIn("updated_at", info)

    def test_write_runner_info_is_safe_under_concurrent_updates(self):
        barrier = threading.Barrier(2)
        errors = []
        original_replace = self.mod.Path.replace

        def synchronized_replace(path, target):
            if path.name.startswith(".runner.json.") and path.suffix == ".tmp":
                barrier.wait(timeout=5)
            return original_replace(path, target)

        self.mod.Path.replace = synchronized_replace
        try:
            def worker(index):
                try:
                    self.mod.write_runner_info(
                        {
                            "pid": os.getpid(),
                            "root": f"/tmp/pulp-{index}",
                            "active_job_id": f"job{index}",
                            "active_branch": f"feature/{index}",
                        }
                    )
                except Exception as exc:  # pragma: no cover - regression guard
                    errors.append(exc)

            threads = [threading.Thread(target=worker, args=(i,)) for i in (1, 2)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
        finally:
            self.mod.Path.replace = original_replace

        self.assertEqual(errors, [])
        info = self.mod.read_runner_info()
        self.assertIn(info["active_job_id"], {"job1", "job2"})
        self.assertIn(info["active_branch"], {"feature/1", "feature/2"})

    def test_update_job_active_targets_tracks_live_state(self):
        job = self.mod.make_job("feature/progress", "4" * 40, "normal", ["mac", "ubuntu"], "run", "full")
        self.mod.ensure_state_dirs()
        self.mod.queue_path().write_text(json.dumps([job], indent=2) + "\n")

        self.mod.update_job_active_targets(
            job["id"],
            {
                "mac": {"status": "pass", "duration_secs": 12.3},
                "ubuntu": {"status": "running"},
            },
        )

        queue = self.mod.load_queue()
        self.assertEqual(queue[0]["active_targets"]["mac"]["status"], "pass")
        self.assertEqual(queue[0]["active_targets"]["ubuntu"]["status"], "running")
        self.assertIn("last_progress_at", queue[0])

    def test_windows_ssh_powershell_command_uses_stdin_eval_wrapper(self):
        cmd = self.mod.windows_ssh_powershell_command("win2")
        self.assertEqual(
            cmd,
            [
                "ssh",
                "win2",
                "powershell",
                "-NoProfile",
                "-NonInteractive",
                "-Command",
                "$script = [Console]::In.ReadToEnd(); Invoke-Expression $script",
            ],
        )

    def test_windows_validation_can_pass_generator_instance(self):
        captured = {}

        def fake_run_logged_command(cmd, **kwargs):
            captured["cmd"] = cmd
            captured["input_text"] = kwargs.get("input_text", "")
            return {
                "timed_out": False,
                "returncode": 0,
                "output": "ok\n",
                "duration_secs": 1.2,
            }

        original_run_logged = self.mod.run_logged_command
        original_probe = self.mod.probe_windows_ssh_cmake_settings
        original_sync = self.mod.sync_job_bundle_to_ssh_host
        original_repo_checkout = self.mod.ensure_windows_remote_repo_checkout
        self.mod.run_logged_command = fake_run_logged_command
        self.mod.probe_windows_ssh_cmake_settings = (
            lambda host, generator, platform, instance: (platform, instance)
        )
        self.mod.sync_job_bundle_to_ssh_host = (
            lambda host, job, report_progress=None, config=None: (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")
        )
        self.mod.ensure_windows_remote_repo_checkout = (
            lambda host, repo_path, remote_url=None, **kwargs: {
                "home_dir": r"C:\Users\danielraffel",
                "repo_path": repo_path,
                "repo_exists": True,
                "git_dir_exists": True,
                "origin_url": remote_url or "https://github.com/danielraffel/pulp",
                "repo_path_unsafe": False,
            }
        )
        try:
            result = self.mod.run_windows_ssh_validation(
                "windows",
                "win",
                "C:\\Pulp",
                {"id": "job123", "branch": "feature/arm", "sha": "a" * 40},
                cmake_generator="Visual Studio 17 2022",
                cmake_platform="ARM64",
                cmake_generator_instance="C:/Program Files/Microsoft Visual Studio/2022/Community",
            )
        finally:
            self.mod.run_logged_command = original_run_logged
            self.mod.probe_windows_ssh_cmake_settings = original_probe
            self.mod.sync_job_bundle_to_ssh_host = original_sync
            self.mod.ensure_windows_remote_repo_checkout = original_repo_checkout

        self.assertEqual(result["status"], "pass")
        self.assertEqual(captured["cmd"][:2], ["ssh", "win"])
        self.assertIn(
            "-DCMAKE_GENERATOR_INSTANCE=$GeneratorInstance",
            captured["input_text"],
        )
        self.assertIn(
            "$GeneratorInstance = 'C:/Program Files/Microsoft Visual Studio/2022/Community'",
            captured["input_text"],
        )
        self.assertIn("$Platform = 'ARM64'", captured["input_text"])
        self.assertIn("$BundleName = 'pulp-ci-job123.bundle'", captured["input_text"])
        self.assertIn("$BundleRef = 'refs/pulp-ci-bundles/job123'", captured["input_text"])
        self.assertIn("$BundleRef`:refs/pulp-ci-bundles/job123", captured["input_text"])

    def test_windows_validation_rejects_missing_repo_probe_payload(self):
        original_sync = self.mod.sync_job_bundle_to_ssh_host
        original_repo_checkout = self.mod.ensure_windows_remote_repo_checkout
        self.mod.sync_job_bundle_to_ssh_host = (
            lambda host, job, report_progress=None, config=None: (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")
        )
        self.mod.ensure_windows_remote_repo_checkout = lambda host, repo_path, remote_url=None, **kwargs: None
        try:
            result = self.mod.run_windows_ssh_validation(
                "windows",
                "win",
                "C:\\Pulp",
                {"id": "job123n", "branch": "feature/null-probe", "sha": "a" * 40},
            )
        finally:
            self.mod.sync_job_bundle_to_ssh_host = original_sync
            self.mod.ensure_windows_remote_repo_checkout = original_repo_checkout

        self.assertEqual(result["status"], "error")
        self.assertIn("no structured payload", result["stderr_tail"])

    def test_windows_validation_passes_config_to_bundle_upload_probe(self):
        captured = {"config": None}
        config = json.loads(self.config_path.read_text())
        config["targets"]["windows"]["host"] = "desktop.example.com"

        def fake_run_logged_command(cmd, **kwargs):
            return {
                "timed_out": False,
                "returncode": 0,
                "output": "ok\n",
                "duration_secs": 1.2,
            }

        def fake_sync_bundle(host, job, report_progress=None, config=None):
            captured["config"] = config
            return (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")

        original_run_logged = self.mod.run_logged_command
        original_probe = self.mod.probe_windows_ssh_cmake_settings
        original_sync = self.mod.sync_job_bundle_to_ssh_host
        original_repo_checkout = self.mod.ensure_windows_remote_repo_checkout
        self.mod.run_logged_command = fake_run_logged_command
        self.mod.probe_windows_ssh_cmake_settings = (
            lambda host, generator, platform, instance: (platform, instance)
        )
        self.mod.sync_job_bundle_to_ssh_host = fake_sync_bundle
        self.mod.ensure_windows_remote_repo_checkout = (
            lambda host, repo_path, remote_url=None, **kwargs: {
                "home_dir": r"C:\Users\danielraffel",
                "repo_path": repo_path,
                "repo_exists": True,
                "git_dir_exists": True,
                "origin_url": remote_url or "https://github.com/danielraffel/pulp",
                "repo_path_unsafe": False,
            }
        )
        try:
            result = self.mod.run_windows_ssh_validation(
                "windows",
                "desktop.example.com",
                "C:\\Pulp",
                {"id": "job123b", "branch": "feature/arm", "sha": "b" * 40},
                config=config,
            )
        finally:
            self.mod.run_logged_command = original_run_logged
            self.mod.probe_windows_ssh_cmake_settings = original_probe
            self.mod.sync_job_bundle_to_ssh_host = original_sync
            self.mod.ensure_windows_remote_repo_checkout = original_repo_checkout

        self.assertEqual(result["status"], "pass")
        self.assertIs(captured["config"], config)

    def test_windows_single_target_rerun_enables_prepared_reuse(self):
        captured = {}

        def fake_run_logged_command(cmd, **kwargs):
            captured["input_text"] = kwargs.get("input_text", "")
            return {
                "timed_out": False,
                "returncode": 0,
                "output": "ok\n",
                "duration_secs": 1.2,
            }

        original_run_logged = self.mod.run_logged_command
        original_probe = self.mod.probe_windows_ssh_cmake_settings
        original_sync = self.mod.sync_job_bundle_to_ssh_host
        original_repo_checkout = self.mod.ensure_windows_remote_repo_checkout
        self.mod.run_logged_command = fake_run_logged_command
        self.mod.probe_windows_ssh_cmake_settings = (
            lambda host, generator, platform, instance: (platform, instance)
        )
        self.mod.sync_job_bundle_to_ssh_host = (
            lambda host, job, report_progress=None, config=None: (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")
        )
        self.mod.ensure_windows_remote_repo_checkout = (
            lambda host, repo_path, remote_url=None, **kwargs: {
                "home_dir": r"C:\Users\danielraffel",
                "repo_path": repo_path,
                "repo_exists": True,
                "git_dir_exists": True,
                "origin_url": remote_url or "https://github.com/danielraffel/pulp",
                "repo_path_unsafe": False,
            }
        )
        try:
            result = self.mod.run_windows_ssh_validation(
                "windows",
                "win",
                "C:\\Pulp",
                {"id": "job127", "branch": "feature/rerun", "sha": "f" * 40, "targets": ["windows"]},
            )
        finally:
            self.mod.run_logged_command = original_run_logged
            self.mod.probe_windows_ssh_cmake_settings = original_probe
            self.mod.sync_job_bundle_to_ssh_host = original_sync
            self.mod.ensure_windows_remote_repo_checkout = original_repo_checkout

        self.assertEqual(result["status"], "pass")
        self.assertIn("$PreparedRoot = Join-Path $CiRoot 'prepared\\windows'", captured["input_text"])
        self.assertIn("$ReusePrepared = $true", captured["input_text"])
        self.assertIn("__PULP_PREPARED__:reused", captured["input_text"])
        self.assertIn("function Remove-DirectoryTreeRobust", captured["input_text"])
        self.assertIn("""cmd.exe /d /c ('rmdir /s /q "{0}"' -f $Path) | Out-Null""", captured["input_text"])
        self.assertIn("""$LongPath = if ($Path.StartsWith('\\\\?\\')) { $Path } else { '\\\\?\\' + $Path }""", captured["input_text"])
        self.assertIn("if (-not (Test-CommitRef $Sha)) {\n        try {\n            Invoke-Native git @('fetch', 'origin')", captured["input_text"])

    def test_windows_smoke_validation_installs_sdk_and_skips_ctest(self):
        captured = {}

        def fake_run_logged_command(cmd, **kwargs):
            captured["input_text"] = kwargs.get("input_text", "")
            return {
                "timed_out": False,
                "returncode": 0,
                "output": "__PULP_VALIDATION__:smoke\n__PULP_TEST_POLICY__:skip\nok\n",
                "duration_secs": 1.2,
            }

        original_run_logged = self.mod.run_logged_command
        original_probe = self.mod.probe_windows_ssh_cmake_settings
        original_sync = self.mod.sync_job_bundle_to_ssh_host
        original_repo_checkout = self.mod.ensure_windows_remote_repo_checkout
        self.mod.run_logged_command = fake_run_logged_command
        self.mod.probe_windows_ssh_cmake_settings = (
            lambda host, generator, platform, instance: (platform, instance)
        )
        self.mod.sync_job_bundle_to_ssh_host = (
            lambda host, job, report_progress=None, config=None: (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")
        )
        self.mod.ensure_windows_remote_repo_checkout = (
            lambda host, repo_path, remote_url=None, **kwargs: {
                "home_dir": r"C:\Users\danielraffel",
                "repo_path": repo_path,
                "repo_exists": True,
                "git_dir_exists": True,
                "origin_url": remote_url or "https://github.com/danielraffel/pulp",
                "repo_path_unsafe": False,
            }
        )
        try:
            result = self.mod.run_windows_ssh_validation(
                "windows",
                "win",
                "C:\\Pulp",
                {"id": "job126", "branch": "feature/smoke", "sha": "e" * 40, "validation": "smoke"},
            )
        finally:
            self.mod.run_logged_command = original_run_logged
            self.mod.probe_windows_ssh_cmake_settings = original_probe
            self.mod.sync_job_bundle_to_ssh_host = original_sync
            self.mod.ensure_windows_remote_repo_checkout = original_repo_checkout

        self.assertEqual(result["status"], "pass")
        self.assertIn("$ValidationMode = 'smoke'", captured["input_text"])
        self.assertIn('Write-Host "__PULP_VALIDATION__:$ValidationMode"', captured["input_text"])
        self.assertIn('Write-Host "__PULP_TEST_POLICY__:skip"', captured["input_text"])
        self.assertIn("-DPULP_BUILD_TESTS=OFF", captured["input_text"])
        self.assertIn("'--install'", captured["input_text"])
        self.assertIn("__PULP_PHASE__:smoke", captured["input_text"])
        self.assertIn("$smokeConfigureArgs = @('-S', $Smoke, '-B', (Join-Path $Smoke 'build'))", captured["input_text"])
        self.assertIn("$smokeConfigureArgs += @('-G', $Generator)", captured["input_text"])
        self.assertIn("$smokeConfigureArgs += @('-A', $Platform)", captured["input_text"])
        self.assertIn('$smokeConfigureArgs += @("-DCMAKE_GENERATOR_INSTANCE=$GeneratorInstance")', captured["input_text"])

    def test_windows_smoke_validation_fails_when_smoke_contract_markers_are_missing(self):
        def fake_run_logged_command(cmd, **kwargs):
            return {
                "timed_out": False,
                "returncode": 0,
                "output": "ok\n",
                "duration_secs": 1.2,
            }

        original_run_logged = self.mod.run_logged_command
        original_probe = self.mod.probe_windows_ssh_cmake_settings
        original_sync = self.mod.sync_job_bundle_to_ssh_host
        original_repo_checkout = self.mod.ensure_windows_remote_repo_checkout
        self.mod.run_logged_command = fake_run_logged_command
        self.mod.probe_windows_ssh_cmake_settings = (
            lambda host, generator, platform, instance: (platform, instance)
        )
        self.mod.sync_job_bundle_to_ssh_host = (
            lambda host, job, report_progress=None, config=None: (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")
        )
        self.mod.ensure_windows_remote_repo_checkout = (
            lambda host, repo_path, remote_url=None, **kwargs: {
                "home_dir": r"C:\Users\danielraffel",
                "repo_path": repo_path,
                "repo_exists": True,
                "git_dir_exists": True,
                "origin_url": remote_url or "https://github.com/danielraffel/pulp",
                "repo_path_unsafe": False,
            }
        )
        try:
            result = self.mod.run_windows_ssh_validation(
                "windows",
                "win",
                "C:\\Pulp",
                {"id": "job126s", "branch": "feature/smoke", "sha": "f" * 40, "validation": "smoke"},
            )
        finally:
            self.mod.run_logged_command = original_run_logged
            self.mod.probe_windows_ssh_cmake_settings = original_probe
            self.mod.sync_job_bundle_to_ssh_host = original_sync
            self.mod.ensure_windows_remote_repo_checkout = original_repo_checkout

        self.assertEqual(result["status"], "fail")
        self.assertIn("Smoke validation contract violated", result["stderr_tail"])

    def test_windows_validation_auto_detects_platform_and_vs_instance(self):
        captured = {}

        def fake_run_logged_command(cmd, **kwargs):
            captured["cmd"] = cmd
            captured["input_text"] = kwargs.get("input_text", "")
            return {
                "timed_out": False,
                "returncode": 0,
                "output": "ok\n",
                "duration_secs": 1.2,
            }

        original_run_logged = self.mod.run_logged_command
        original_probe = self.mod.probe_windows_ssh_cmake_settings
        original_sync = self.mod.sync_job_bundle_to_ssh_host
        original_repo_checkout = self.mod.ensure_windows_remote_repo_checkout
        self.mod.run_logged_command = fake_run_logged_command
        self.mod.probe_windows_ssh_cmake_settings = (
            lambda host, generator, platform, instance: (
                "ARM64",
                "C:/Program Files/Microsoft Visual Studio/2022/Community",
            )
        )
        self.mod.sync_job_bundle_to_ssh_host = (
            lambda host, job, report_progress=None, config=None: (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")
        )
        self.mod.ensure_windows_remote_repo_checkout = (
            lambda host, repo_path, remote_url=None, **kwargs: {
                "home_dir": r"C:\Users\danielraffel",
                "repo_path": repo_path,
                "repo_exists": True,
                "git_dir_exists": True,
                "origin_url": remote_url or "https://github.com/danielraffel/pulp",
                "repo_path_unsafe": False,
            }
        )
        try:
            result = self.mod.run_windows_ssh_validation(
                "windows",
                "win",
                "C:\\Pulp",
                {"id": "job123", "branch": "feature/arm", "sha": "b" * 40},
            )
        finally:
            self.mod.run_logged_command = original_run_logged
            self.mod.probe_windows_ssh_cmake_settings = original_probe
            self.mod.sync_job_bundle_to_ssh_host = original_sync
            self.mod.ensure_windows_remote_repo_checkout = original_repo_checkout

        self.assertEqual(result["status"], "pass")
        self.assertIn("$Platform = 'ARM64'", captured["input_text"])
        self.assertIn(
            "$GeneratorInstance = 'C:/Program Files/Microsoft Visual Studio/2022/Community'",
            captured["input_text"],
        )
        self.assertIn("$BundleName = 'pulp-ci-job123.bundle'", captured["input_text"])

    def test_windows_validation_recovers_abandoned_host_mutex(self):
        captured = {}

        def fake_run_logged_command(cmd, **kwargs):
            captured["input_text"] = kwargs.get("input_text", "")
            return {
                "timed_out": False,
                "returncode": 0,
                "output": "ok\n",
                "duration_secs": 1.2,
            }

        original_run_logged = self.mod.run_logged_command
        original_probe = self.mod.probe_windows_ssh_cmake_settings
        original_sync = self.mod.sync_job_bundle_to_ssh_host
        original_repo_checkout = self.mod.ensure_windows_remote_repo_checkout
        self.mod.run_logged_command = fake_run_logged_command
        self.mod.probe_windows_ssh_cmake_settings = (
            lambda host, generator, platform, instance: (platform, instance)
        )
        self.mod.sync_job_bundle_to_ssh_host = (
            lambda host, job, report_progress=None, config=None: (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")
        )
        self.mod.ensure_windows_remote_repo_checkout = (
            lambda host, repo_path, remote_url=None, **kwargs: {
                "home_dir": r"C:\Users\danielraffel",
                "repo_path": repo_path,
                "repo_exists": True,
                "git_dir_exists": True,
                "origin_url": remote_url or "https://github.com/danielraffel/pulp",
                "repo_path_unsafe": False,
            }
        )
        try:
            result = self.mod.run_windows_ssh_validation(
                "windows",
                "win",
                "C:\\Pulp",
                {"id": "job124", "branch": "feature/mutex", "sha": "c" * 40},
            )
        finally:
            self.mod.run_logged_command = original_run_logged
            self.mod.probe_windows_ssh_cmake_settings = original_probe
            self.mod.sync_job_bundle_to_ssh_host = original_sync
            self.mod.ensure_windows_remote_repo_checkout = original_repo_checkout

        self.assertEqual(result["status"], "pass")
        self.assertIn("function Wait-HostMutex", captured["input_text"])
        self.assertIn("AbandonedMutexException", captured["input_text"])
        self.assertIn("Recovered abandoned host validation lock: $MutexName", captured["input_text"])
        self.assertIn('Write-Host "__PULP_VALIDATOR_PID__:$PID"', captured["input_text"])
        self.assertIn('Write-Host "__PULP_VALIDATOR_STARTED__:$ValidatorStartedAt"', captured["input_text"])

    def test_reclaim_stale_remote_validators_cleans_targeted_windows_pid(self):
        job, _created = self.mod.enqueue_job(
            "feature/stale",
            "d" * 40,
            "normal",
            ["windows"],
            "run",
            "full",
        )

        with self.mod.file_lock(self.mod.queue_lock_path(), blocking=True):
            queue = self.mod.load_queue_unlocked()
            stored = self.mod.find_job_unlocked(queue, job["id"])
            self.assertIsNotNone(stored)
            stored["status"] = "running"
            stored["runner"] = {"pid": 999999, "root": "/dead-runner"}
            stored["active_targets"] = {
                "windows": {
                    "status": "running",
                    "host": "win",
                    "validator_pid": 4321,
                    "validator_started_at": "2026-04-02T04:00:00+00:00",
                    "phase": "waiting-lock",
                }
            }
            self.mod.save_queue_unlocked(queue)

        with mock.patch.object(
            self.mod,
            "cleanup_stale_windows_validator",
            return_value={"found": True, "matched": True, "killed": True, "pid": 4321},
        ) as cleanup:
            reclaimed = self.mod.reclaim_stale_remote_validators({})

        self.assertEqual(reclaimed, 1)
        cleanup.assert_called_once_with("win", 4321, "2026-04-02T04:00:00+00:00")

        refreshed = self.mod.load_job(job["id"])
        self.assertIsNotNone(refreshed)
        state = refreshed["active_targets"]["windows"]
        self.assertEqual(state["cleanup_status"], "killed")
        self.assertIn("cleanup_completed_at", state)
        self.assertNotIn("validator_pid", state)
        self.assertNotIn("validator_started_at", state)

    def test_windows_validation_checks_commit_refs_quietly(self):
        captured = {}

        def fake_run_logged_command(cmd, **kwargs):
            captured["input_text"] = kwargs.get("input_text", "")
            return {
                "timed_out": False,
                "returncode": 0,
                "output": "ok\n",
                "duration_secs": 1.2,
            }

        original_run_logged = self.mod.run_logged_command
        original_probe = self.mod.probe_windows_ssh_cmake_settings
        original_sync = self.mod.sync_job_bundle_to_ssh_host
        original_repo_checkout = self.mod.ensure_windows_remote_repo_checkout
        self.mod.run_logged_command = fake_run_logged_command
        self.mod.probe_windows_ssh_cmake_settings = (
            lambda host, generator, platform, instance: (platform, instance)
        )
        self.mod.sync_job_bundle_to_ssh_host = (
            lambda host, job, report_progress=None, config=None: (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")
        )
        self.mod.ensure_windows_remote_repo_checkout = (
            lambda host, repo_path, remote_url=None, **kwargs: {
                "home_dir": r"C:\Users\danielraffel",
                "repo_path": repo_path,
                "repo_exists": True,
                "git_dir_exists": True,
                "origin_url": remote_url or "https://github.com/danielraffel/pulp",
                "repo_path_unsafe": False,
            }
        )
        try:
            result = self.mod.run_windows_ssh_validation(
                "windows",
                "win",
                "C:\\Pulp",
                {"id": "job125", "branch": "feature/commit-probe", "sha": "d" * 40},
            )
        finally:
            self.mod.run_logged_command = original_run_logged
            self.mod.probe_windows_ssh_cmake_settings = original_probe
            self.mod.sync_job_bundle_to_ssh_host = original_sync
            self.mod.ensure_windows_remote_repo_checkout = original_repo_checkout

        self.assertEqual(result["status"], "pass")
        self.assertIn("git rev-parse --verify --quiet", captured["input_text"])

    def test_windows_validation_fetches_branch_with_explicit_refspec(self):
        captured = {}

        def fake_run_logged_command(cmd, **kwargs):
            captured["input_text"] = kwargs.get("input_text", "")
            return {
                "timed_out": False,
                "returncode": 0,
                "output": "ok\n",
                "duration_secs": 1.2,
            }

        original_run_logged = self.mod.run_logged_command
        original_probe = self.mod.probe_windows_ssh_cmake_settings
        original_sync = self.mod.sync_job_bundle_to_ssh_host
        original_repo_checkout = self.mod.ensure_windows_remote_repo_checkout
        self.mod.run_logged_command = fake_run_logged_command
        self.mod.probe_windows_ssh_cmake_settings = (
            lambda host, generator, platform, instance: (platform, instance)
        )
        self.mod.sync_job_bundle_to_ssh_host = (
            lambda host, job, report_progress=None, config=None: (f"pulp-ci-{job['id']}.bundle", f"refs/pulp-ci-bundles/{job['id']}")
        )
        self.mod.ensure_windows_remote_repo_checkout = (
            lambda host, repo_path, remote_url=None, **kwargs: {
                "home_dir": r"C:\Users\danielraffel",
                "repo_path": repo_path,
                "repo_exists": True,
                "git_dir_exists": True,
                "origin_url": remote_url or "https://github.com/danielraffel/pulp",
                "repo_path_unsafe": False,
            }
        )
        try:
            result = self.mod.run_windows_ssh_validation(
                "windows",
                "win",
                "C:\\Pulp",
                {"id": "job126", "branch": "feature/refspec", "sha": "e" * 40},
            )
        finally:
            self.mod.run_logged_command = original_run_logged
            self.mod.probe_windows_ssh_cmake_settings = original_probe
            self.mod.sync_job_bundle_to_ssh_host = original_sync
            self.mod.ensure_windows_remote_repo_checkout = original_repo_checkout

        self.assertEqual(result["status"], "pass")
        self.assertIn("refs/heads/$Branch`:refs/remotes/origin/$Branch", captured["input_text"])
        self.assertIn("$BundleName = 'pulp-ci-job126.bundle'", captured["input_text"])
        self.assertIn("$BundleRef`:refs/pulp-ci-bundles/job126", captured["input_text"])

    def test_sync_job_bundle_to_ssh_host_uses_scp_and_keeps_local_bundle(self):
        bundle_path = self.state_dir / "bundles" / "job777.bundle"
        captured = {}

        def fake_create_job_bundle(job):
            bundle_path.parent.mkdir(parents=True, exist_ok=True)
            bundle_path.write_text("bundle")
            return bundle_path

        class FakeProc:
            def __init__(self):
                self.returncode = 0

            def communicate(self, timeout=None):
                return ("", "")

            def kill(self):
                self.returncode = -9

            def terminate(self):
                self.returncode = 0

        original_create = self.mod.create_job_bundle
        original_popen = self.mod.subprocess.Popen
        self.mod.create_job_bundle = fake_create_job_bundle
        def fake_popen(cmd, **kwargs):
            captured["cmd"] = cmd
            captured["kwargs"] = kwargs
            proc = FakeProc()
            captured["proc"] = proc
            return proc
        self.mod.subprocess.Popen = fake_popen
        try:
            remote_name, bundle_ref = self.mod.sync_job_bundle_to_ssh_host(
                "win",
                {"id": "job777", "sha": "f" * 40},
            )
        finally:
            self.mod.create_job_bundle = original_create
            self.mod.subprocess.Popen = original_popen

        self.assertEqual(remote_name, "pulp-ci-job777.bundle")
        self.assertEqual(bundle_ref, "refs/pulp-ci-bundles/job777")
        self.assertEqual(captured["cmd"], ["scp", str(bundle_path), "win:pulp-ci-job777.bundle"])
        self.assertTrue(bundle_path.exists())

    def test_sync_job_bundle_to_ssh_host_uses_probe_without_explicit_config(self):
        bundle_path = self.state_dir / "bundles" / "job778.bundle"
        captured = {"timeouts": 0, "probe_config": None}

        def fake_create_job_bundle(job):
            bundle_path.parent.mkdir(parents=True, exist_ok=True)
            bundle_path.write_text("bundle")
            return bundle_path

        class FakeProc:
            def __init__(self):
                self.returncode = 0

            def communicate(self, timeout=None):
                if captured["timeouts"] == 0:
                    captured["timeouts"] += 1
                    raise subprocess.TimeoutExpired(["scp"], timeout)
                return ("", "")

            def kill(self):
                self.returncode = -9

            def terminate(self):
                self.returncode = 0

        original_create = self.mod.create_job_bundle
        original_popen = self.mod.subprocess.Popen
        original_probe = self.mod.probe_uploaded_bundle_size
        self.mod.create_job_bundle = fake_create_job_bundle
        self.mod.subprocess.Popen = lambda *args, **kwargs: FakeProc()
        def fake_probe(host, remote_name, *, config):
            captured["probe_config"] = config
            return bundle_path.stat().st_size

        self.mod.probe_uploaded_bundle_size = fake_probe
        try:
            remote_name, bundle_ref = self.mod.sync_job_bundle_to_ssh_host(
                "win",
                {"id": "job778", "sha": "a" * 40},
            )
        finally:
            self.mod.create_job_bundle = original_create
            self.mod.subprocess.Popen = original_popen
            self.mod.probe_uploaded_bundle_size = original_probe

        self.assertEqual(remote_name, "pulp-ci-job778.bundle")
        self.assertEqual(bundle_ref, "refs/pulp-ci-bundles/job778")
        self.assertEqual(captured["timeouts"], 1)
        self.assertIsInstance(captured["probe_config"], dict)

    def test_sync_job_bundle_to_ssh_host_preserves_windows_target_metadata_for_probe(self):
        bundle_path = self.state_dir / "bundles" / "job779.bundle"
        captured = {"timeouts": 0, "probe_config": None}
        config = json.loads(self.config_path.read_text())
        config["targets"]["windows"]["enabled"] = True
        config["targets"]["windows"]["host"] = "desktop.example.com"

        def fake_create_job_bundle(job):
            bundle_path.parent.mkdir(parents=True, exist_ok=True)
            bundle_path.write_text("bundle")
            return bundle_path

        class FakeProc:
            def __init__(self):
                self.returncode = 0

            def communicate(self, timeout=None):
                if captured["timeouts"] == 0:
                    captured["timeouts"] += 1
                    raise subprocess.TimeoutExpired(["scp"], timeout)
                return ("", "")

            def kill(self):
                self.returncode = -9

            def terminate(self):
                self.returncode = 0

        def fake_probe(host, remote_name, *, config):
            captured["probe_config"] = config
            self.assertTrue(self.mod.ssh_host_uses_windows_shell(config, host))
            return bundle_path.stat().st_size

        original_create = self.mod.create_job_bundle
        original_popen = self.mod.subprocess.Popen
        original_probe = self.mod.probe_uploaded_bundle_size
        self.mod.create_job_bundle = fake_create_job_bundle
        self.mod.subprocess.Popen = lambda *args, **kwargs: FakeProc()
        self.mod.probe_uploaded_bundle_size = fake_probe
        try:
            remote_name, bundle_ref = self.mod.sync_job_bundle_to_ssh_host(
                "desktop.example.com",
                {"id": "job779", "sha": "b" * 40},
                config=config,
            )
        finally:
            self.mod.create_job_bundle = original_create
            self.mod.subprocess.Popen = original_popen
            self.mod.probe_uploaded_bundle_size = original_probe

        self.assertEqual(remote_name, "pulp-ci-job779.bundle")
        self.assertEqual(bundle_ref, "refs/pulp-ci-bundles/job779")
        self.assertEqual(captured["timeouts"], 1)
        self.assertEqual(captured["probe_config"], config)

    def test_sync_job_bundle_to_ssh_host_uses_submission_config_for_probe(self):
        bundle_path = self.state_dir / "bundles" / "job780.bundle"
        captured = {"timeouts": 0, "probe_config": None}
        submitted_config = {
            "targets": {
                "windows": {
                    "type": "ssh",
                    "enabled": True,
                    "host": "desktop.example.com",
                    "repo_path": r"C:\Users\danielraffel\pulp-validate",
                }
            },
            "defaults": {},
        }
        submitted_path = Path(self.tmpdir.name) / "submission-config.json"
        submitted_path.write_text(json.dumps(submitted_config) + "\n")

        def fake_create_job_bundle(job):
            bundle_path.parent.mkdir(parents=True, exist_ok=True)
            bundle_path.write_text("bundle")
            return bundle_path

        class FakeProc:
            def __init__(self):
                self.returncode = 0

            def communicate(self, timeout=None):
                if captured["timeouts"] == 0:
                    captured["timeouts"] += 1
                    raise subprocess.TimeoutExpired(["scp"], timeout)
                return ("", "")

            def kill(self):
                self.returncode = -9

            def terminate(self):
                self.returncode = 0

        def fake_probe(host, remote_name, *, config):
            captured["probe_config"] = config
            self.assertTrue(self.mod.ssh_host_uses_windows_shell(config, host))
            return bundle_path.stat().st_size

        original_create = self.mod.create_job_bundle
        original_popen = self.mod.subprocess.Popen
        original_probe = self.mod.probe_uploaded_bundle_size
        self.mod.create_job_bundle = fake_create_job_bundle
        self.mod.subprocess.Popen = lambda *args, **kwargs: FakeProc()
        self.mod.probe_uploaded_bundle_size = fake_probe
        try:
            remote_name, bundle_ref = self.mod.sync_job_bundle_to_ssh_host(
                "desktop.example.com",
                {
                    "id": "job780",
                    "sha": "c" * 40,
                    "submission": {"config_path": str(submitted_path)},
                },
            )
        finally:
            self.mod.create_job_bundle = original_create
            self.mod.subprocess.Popen = original_popen
            self.mod.probe_uploaded_bundle_size = original_probe

        self.assertEqual(remote_name, "pulp-ci-job780.bundle")
        self.assertEqual(bundle_ref, "refs/pulp-ci-bundles/job780")
        self.assertEqual(captured["timeouts"], 1)
        self.assertEqual(captured["probe_config"]["targets"]["windows"]["host"], "desktop.example.com")

    def test_create_job_bundle_reuses_existing_artifact_across_threads(self):
        job = {"id": "job-concurrent", "sha": "a" * 40}
        bundle_path = self.state_dir / "bundles" / "job-concurrent.bundle"
        create_calls = []
        original_run = self.mod.subprocess.run

        def fake_run(cmd, cwd=None, check=None, **kwargs):
            if cmd[:3] == ["git", "bundle", "create"]:
                create_calls.append(cmd)
                bundle_path.parent.mkdir(parents=True, exist_ok=True)
                bundle_path.write_text("bundle")
            return SimpleNamespace(returncode=0, stdout="", stderr="")

        self.mod.subprocess.run = fake_run
        try:
            results = []

            def worker():
                results.append(self.mod.create_job_bundle(job))

            threads = [threading.Thread(target=worker) for _ in range(2)]
            for thread in threads:
                thread.start()
            for thread in threads:
                thread.join()
        finally:
            self.mod.subprocess.run = original_run

        self.assertEqual(len(create_calls), 1)
        self.assertEqual(results, [bundle_path, bundle_path])
        self.assertTrue(bundle_path.exists())

    def test_posix_validation_fetches_uploaded_bundle_first(self):
        captured = {}

        def fake_sync_bundle(host, job, report_progress=None, config=None):
            return ("pulp-ci-job888.bundle", "refs/pulp-ci-bundles/job888")

        def fake_run_logged_command(cmd, **kwargs):
            captured["cmd"] = cmd
            return {
                "timed_out": False,
                "returncode": 0,
                "output": "ok\n",
                "duration_secs": 1.2,
            }

        original_sync = self.mod.sync_job_bundle_to_ssh_host
        original_run_logged = self.mod.run_logged_command
        self.mod.sync_job_bundle_to_ssh_host = fake_sync_bundle
        self.mod.run_logged_command = fake_run_logged_command
        try:
            result = self.mod.run_posix_ssh_validation(
                "ubuntu",
                "ubuntu",
                "/tmp/pulp",
                {"id": "job888", "branch": "feature/bundle", "sha": "1" * 40},
            )
        finally:
            self.mod.sync_job_bundle_to_ssh_host = original_sync
            self.mod.run_logged_command = original_run_logged

        self.assertEqual(result["status"], "pass")
        remote_cmd = captured["cmd"][-1]
        self.assertIn("bundle-sync", remote_cmd)
        self.assertIn('bundle="$HOME/$bundle_name"', remote_cmd)
        self.assertIn('prepared_root="$HOME/.local/state/pulp/local-ci/prepared/ubuntu/full"', remote_cmd)
        self.assertIn('PULP_VALIDATE_REUSE_PREPARED="$reuse_prepared"', remote_cmd)
        self.assertIn('script="$PWD/$script_name"', remote_cmd)
        self.assertIn('git fetch "$bundle" "$bundle_ref:refs/remotes/origin/$branch"', remote_cmd)
        self.assertIn('git show "$sha:validate-build.sh" > "$script"', remote_cmd)
        self.assertIn('bash "$script" --quiet --keep-worktree --ref "$sha"', remote_cmd)

    def test_posix_smoke_validation_runs_sha_pinned_script_with_smoke_flag(self):
        captured = {}

        def fake_sync_bundle(host, job, report_progress=None, config=None):
            return ("pulp-ci-job889.bundle", "refs/pulp-ci-bundles/job889")

        def fake_run_logged_command(cmd, **kwargs):
            captured["cmd"] = cmd
            return {
                "timed_out": False,
                "returncode": 0,
                "output": "__PULP_VALIDATION__:smoke\n__PULP_TEST_POLICY__:skip\nok\n",
                "duration_secs": 1.2,
            }

        original_sync = self.mod.sync_job_bundle_to_ssh_host
        original_run_logged = self.mod.run_logged_command
        self.mod.sync_job_bundle_to_ssh_host = fake_sync_bundle
        self.mod.run_logged_command = fake_run_logged_command
        try:
            result = self.mod.run_posix_ssh_validation(
                "ubuntu",
                "ubuntu",
                "/tmp/pulp",
                {"id": "job889", "branch": "feature/smoke", "sha": "2" * 40, "validation": "smoke"},
            )
        finally:
            self.mod.sync_job_bundle_to_ssh_host = original_sync
            self.mod.run_logged_command = original_run_logged

        self.assertEqual(result["status"], "pass")
        remote_cmd = captured["cmd"][-1]
        self.assertIn('script_name=.pulp-ci-validate-job889.sh', remote_cmd)
        self.assertIn('prepared_root="$HOME/.local/state/pulp/local-ci/prepared/ubuntu/smoke"', remote_cmd)
        self.assertIn('git show "$sha:validate-build.sh" > "$script"', remote_cmd)
        self.assertIn(
            'PULP_EXPECT_SMOKE=1 bash "$script" --quiet --keep-worktree --ref "$sha" --smoke --no-tests',
            remote_cmd,
        )

    def test_posix_smoke_validation_fails_when_smoke_contract_markers_are_missing(self):
        def fake_sync_bundle(host, job, report_progress=None, config=None):
            return ("pulp-ci-job890.bundle", "refs/pulp-ci-bundles/job890")

        def fake_run_logged_command(cmd, **kwargs):
            return {
                "timed_out": False,
                "returncode": 0,
                "output": "ok\n",
                "duration_secs": 1.2,
            }

        original_sync = self.mod.sync_job_bundle_to_ssh_host
        original_run_logged = self.mod.run_logged_command
        self.mod.sync_job_bundle_to_ssh_host = fake_sync_bundle
        self.mod.run_logged_command = fake_run_logged_command
        try:
            result = self.mod.run_posix_ssh_validation(
                "ubuntu",
                "ubuntu",
                "/tmp/pulp",
                {"id": "job890", "branch": "feature/smoke", "sha": "3" * 40, "validation": "smoke"},
            )
        finally:
            self.mod.sync_job_bundle_to_ssh_host = original_sync
            self.mod.run_logged_command = original_run_logged

        self.assertEqual(result["status"], "fail")
        self.assertIn("Smoke validation contract violated", result["stderr_tail"])

    def test_probe_windows_ssh_cmake_settings_parses_remote_json(self):
        class FakeCompleted:
            def __init__(self):
                self.returncode = 0
                self.stdout = '\n{"platform":"ARM64","generator_instance":"C:/Program Files/Microsoft Visual Studio/2022/Community"}\n'

        original_run = self.mod.subprocess.run
        self.mod.subprocess.run = lambda *args, **kwargs: FakeCompleted()
        try:
            platform, instance = self.mod.probe_windows_ssh_cmake_settings(
                "win",
                "Visual Studio 17 2022",
                "",
                "",
            )
        finally:
            self.mod.subprocess.run = original_run

        self.assertEqual(platform, "ARM64")
        self.assertEqual(instance, "C:/Program Files/Microsoft Visual Studio/2022/Community")

    def test_probe_windows_repo_checkout_checks_origin_safely(self):
        captured = {}

        def fake_run(host, script, timeout=0):
            captured["script"] = script
            return SimpleNamespace(
                returncode=0,
                stdout='{"home_dir":"C:/Users/danielraffel","repo_path":"C:/Users/danielraffel/pulp-validate","repo_exists":true,"git_dir_exists":true,"head_exists":true,"setup_exists":true,"origin_url":""}',
                stderr="",
            )

        original_run = self.mod.run_windows_ssh_powershell
        self.mod.run_windows_ssh_powershell = fake_run
        try:
            result = self.mod.probe_windows_repo_checkout("win", r"C:\Users\danielraffel\pulp-validate")
        finally:
            self.mod.run_windows_ssh_powershell = original_run

        self.assertEqual(result["origin_url"], "")
        self.assertIn("git -C $Repo remote 2>$null", captured["script"])
        self.assertIn("Where-Object { $_ -eq 'origin' }", captured["script"])
        self.assertIn("git -C $Repo rev-parse --verify --quiet HEAD 2>$null", captured["script"])

    def test_ensure_windows_remote_repo_checkout_adds_origin_only_when_missing(self):
        captured = {}

        def fake_run(host, script, timeout=0):
            captured["script"] = script
            return SimpleNamespace(
                returncode=0,
                stdout='{"home_dir":"C:/Users/danielraffel","repo_path":"C:/Users/danielraffel/pulp-validate","repo_exists":true,"git_dir_exists":true,"head_exists":true,"setup_exists":true,"origin_url":"https://github.com/danielraffel/pulp"}',
                stderr="",
            )

        original_run = self.mod.run_windows_ssh_powershell
        original_parse = self.mod.parse_windows_ssh_json
        self.mod.run_windows_ssh_powershell = fake_run
        self.mod.parse_windows_ssh_json = lambda stdout: json.loads(stdout)
        try:
            result = self.mod.ensure_windows_remote_repo_checkout(
                "win",
                r"C:\Users\danielraffel\pulp-validate",
                remote_url="https://github.com/danielraffel/pulp",
            )
        finally:
            self.mod.run_windows_ssh_powershell = original_run
            self.mod.parse_windows_ssh_json = original_parse

        self.assertEqual(result["origin_url"], "https://github.com/danielraffel/pulp")
        self.assertIn("git -C $Repo remote 2>$null", captured["script"])
        self.assertIn("if (-not $OriginUrl -and $RemoteUrl)", captured["script"])
        self.assertIn("$NeedsMaterialize = $false", captured["script"])

    def test_ensure_windows_remote_repo_checkout_materializes_incomplete_checkout(self):
        captured = {}

        probe = {
            "home_dir": "C:/Users/danielraffel",
            "repo_path": "C:/Users/danielraffel/pulp-validate",
            "repo_exists": True,
            "git_dir_exists": True,
            "head_exists": True,
            "setup_exists": False,
            "origin_url": "https://github.com/danielraffel/pulp",
        }

        def fake_run(host, script, timeout=0):
            captured["script"] = script
            return SimpleNamespace(
                returncode=0,
                stdout='{"home_dir":"C:/Users/danielraffel","repo_path":"C:/Users/danielraffel/pulp-validate","repo_exists":true,"git_dir_exists":true,"head":"abc123","head_exists":true,"setup_path":"C:/Users/danielraffel/pulp-validate/setup.sh","setup_exists":true,"origin_url":"https://github.com/danielraffel/pulp"}',
                stderr="",
            )

        original_probe = self.mod.probe_windows_repo_checkout
        original_run = self.mod.run_windows_ssh_powershell
        original_parse = self.mod.parse_windows_ssh_json
        self.mod.probe_windows_repo_checkout = lambda host, repo_path: probe
        self.mod.run_windows_ssh_powershell = fake_run
        self.mod.parse_windows_ssh_json = lambda stdout: json.loads(stdout)
        try:
            result = self.mod.ensure_windows_remote_repo_checkout(
                "win",
                r"C:\Users\danielraffel\pulp-validate",
                remote_url="https://github.com/danielraffel/pulp",
            )
        finally:
            self.mod.probe_windows_repo_checkout = original_probe
            self.mod.run_windows_ssh_powershell = original_run
            self.mod.parse_windows_ssh_json = original_parse

        self.assertTrue(result["setup_exists"])
        self.assertEqual(result["origin_url"], "https://github.com/danielraffel/pulp")
        self.assertIn("$NeedsMaterialize = $true", captured["script"])
        self.assertIn("[Environment]::SetEnvironmentVariable('GIT_LFS_SKIP_SMUDGE', '1', 'Process')", captured["script"])
        self.assertIn("git -C $Repo fetch --depth 1 origin main", captured["script"])
        self.assertIn("git -C $Repo checkout --force -B main FETCH_HEAD", captured["script"])

    def test_ensure_windows_remote_repo_checkout_prefers_bundle_materialization(self):
        captured = {}

        probe = {
            "home_dir": "C:/Users/danielraffel",
            "repo_path": "C:/Users/danielraffel/pulp-validate",
            "repo_exists": True,
            "git_dir_exists": True,
            "head_exists": False,
            "setup_exists": False,
            "origin_url": "https://github.com/danielraffel/pulp",
        }

        def fake_run(host, script, timeout=0):
            captured["script"] = script
            return SimpleNamespace(
                returncode=0,
                stdout='{"home_dir":"C:/Users/danielraffel","repo_path":"C:/Users/danielraffel/pulp-validate","repo_exists":true,"git_dir_exists":true,"head":"abc123","head_exists":true,"setup_path":"C:/Users/danielraffel/pulp-validate/setup.sh","setup_exists":true,"origin_url":"https://github.com/danielraffel/pulp"}',
                stderr="",
            )

        original_probe = self.mod.probe_windows_repo_checkout
        original_run = self.mod.run_windows_ssh_powershell
        original_parse = self.mod.parse_windows_ssh_json
        self.mod.probe_windows_repo_checkout = lambda host, repo_path: probe
        self.mod.run_windows_ssh_powershell = fake_run
        self.mod.parse_windows_ssh_json = lambda stdout: json.loads(stdout)
        try:
            result = self.mod.ensure_windows_remote_repo_checkout(
                "win",
                r"C:\Users\danielraffel\pulp-validate",
                remote_url="https://github.com/danielraffel/pulp",
                bundle_name="pulp-ci-123.bundle",
                bundle_ref="refs/pulp-ci-bundles/123",
            )
        finally:
            self.mod.probe_windows_repo_checkout = original_probe
            self.mod.run_windows_ssh_powershell = original_run
            self.mod.parse_windows_ssh_json = original_parse

        self.assertTrue(result["setup_exists"])
        self.assertIn("$Bundle = 'pulp-ci-123.bundle'", captured["script"])
        self.assertIn("$BundleRef = 'refs/pulp-ci-bundles/123'", captured["script"])
        self.assertIn("[Environment]::SetEnvironmentVariable('GIT_LFS_SKIP_SMUDGE', '1', 'Process')", captured["script"])
        self.assertIn("git -C $Repo fetch $BundlePath \"$BundleRef`:refs/pulp-ci-bundles/source\"", captured["script"])

    def test_windows_repo_checkout_ready_requires_head_and_setup(self):
        self.assertTrue(
            self.mod.windows_repo_checkout_ready(
                {"git_dir_exists": True, "head_exists": True, "setup_exists": True, "repo_path_unsafe": False}
            )
        )
        self.assertFalse(
            self.mod.windows_repo_checkout_ready(
                {"git_dir_exists": True, "head_exists": False, "setup_exists": True, "repo_path_unsafe": False}
            )
        )
        self.assertFalse(
            self.mod.windows_repo_checkout_ready(
                {"git_dir_exists": True, "head_exists": True, "setup_exists": False, "repo_path_unsafe": False}
            )
        )

    def test_windows_repo_checkout_detail_reports_empty_or_incomplete_checkout(self):
        empty_detail = self.mod.windows_repo_checkout_detail(
            {
                "repo_path": r"C:\Users\danielraffel\pulp-validate",
                "git_dir_exists": True,
                "head_exists": False,
                "setup_exists": False,
                "origin_url": "https://github.com/danielraffel/pulp",
            }
        )
        self.assertIn("empty git repo", empty_detail)

        incomplete_detail = self.mod.windows_repo_checkout_detail(
            {
                "repo_path": r"C:\Users\danielraffel\pulp-validate",
                "git_dir_exists": True,
                "head_exists": True,
                "setup_exists": False,
                "origin_url": "https://github.com/danielraffel/pulp",
            }
        )
        self.assertIn("checkout incomplete; setup.sh missing", incomplete_detail)

    def test_run_logged_command_starts_reader_before_writing_input(self):
        read_started = threading.Event()
        read_finished = threading.Event()
        writes = []

        class FakeStdout:
            def __iter__(self):
                read_started.set()
                yield "ready\n"
                read_finished.set()

            def close(self):
                read_finished.set()

        class FakeStdin:
            def write(self, text):
                if not read_started.wait(timeout=1):
                    raise TimeoutError("reader did not start before stdin write")
                writes.append(text)

            def close(self):
                writes.append("<closed>")

        class FakeProc:
            def __init__(self):
                self.stdin = FakeStdin()
                self.stdout = FakeStdout()

            def poll(self):
                return 0 if read_finished.is_set() else None

            def wait(self, timeout=None):
                self.poll()
                read_finished.wait(timeout=timeout)
                return 0

            def kill(self):
                read_finished.set()

        original_popen = self.mod.subprocess.Popen
        self.mod.subprocess.Popen = lambda *args, **kwargs: FakeProc()
        try:
            result = self.mod.run_logged_command(["ssh", "win2"], input_text="payload", timeout=5)
        finally:
            self.mod.subprocess.Popen = original_popen

        self.assertFalse(result["timed_out"])
        self.assertEqual(result["returncode"], 0)
        self.assertEqual(result["output"], "ready\n")
        self.assertEqual(writes, ["payload", "<closed>"])

    def test_parse_progress_marker_detects_phase_wait_and_smoke_contract(self):
        self.assertEqual(
            self.mod.parse_progress_marker("__PULP_PHASE__:build\n"),
            {"phase": "build"},
        )
        self.assertEqual(
            self.mod.parse_progress_marker("__PULP_WAIT__:host-lock\n"),
            {"wait_reason": "host-lock"},
        )
        self.assertEqual(
            self.mod.parse_progress_marker("__PULP_VALIDATION__:smoke\n"),
            {"validation_mode": "smoke"},
        )
        self.assertEqual(
            self.mod.parse_progress_marker("__PULP_TEST_POLICY__:skip\n"),
            {"test_policy": "skip"},
        )
        self.assertEqual(
            self.mod.parse_progress_marker("__PULP_PREPARED__:reused\n"),
            {"prepared_state": "reused"},
        )
        self.assertEqual(
            self.mod.parse_progress_marker("__PULP_VALIDATOR_PID__:4321\n"),
            {"validator_pid": 4321},
        )
        self.assertEqual(
            self.mod.parse_progress_marker("__PULP_VALIDATOR_STARTED__:2026-04-02T04:00:00+00:00\n"),
            {"validator_started_at": "2026-04-02T04:00:00+00:00"},
        )
        self.assertEqual(self.mod.parse_progress_marker("normal output\n"), {})

    def test_validate_build_preserves_original_args_for_lock_reexec(self):
        text = VALIDATE_BUILD_PATH.read_text()
        self.assertIn('ORIGINAL_ARGS=("$@")', text)
        self.assertIn('acquire_validation_lock "${ORIGINAL_ARGS[@]}"', text)

    def test_run_local_validation_uses_prepared_root_for_single_target_reruns(self):
        captured = {}

        def fake_run_logged_command(cmd, **kwargs):
            captured["cmd"] = cmd
            return {
                "timed_out": False,
                "returncode": 0,
                "output": "ok\n",
                "duration_secs": 1.2,
            }

        original_run_logged = self.mod.run_logged_command
        self.mod.run_logged_command = fake_run_logged_command
        try:
            result = self.mod.run_local_validation(
                {"id": "job501", "branch": "feature/local", "sha": "5" * 40, "targets": ["mac"]}
            )
        finally:
            self.mod.run_logged_command = original_run_logged

        self.assertEqual(result["status"], "pass")
        cmd = captured["cmd"]
        self.assertEqual(cmd[0], "env")
        self.assertIn("PULP_VALIDATE_REUSE_PREPARED=1", cmd)
        self.assertTrue(
            any(arg.startswith("PULP_VALIDATE_ROOT_OVERRIDE=") for arg in cmd),
            msg=f"missing prepared root override in {cmd}",
        )
        self.assertIn("--keep-worktree", cmd)

    def test_run_logged_command_keeps_progress_markers_in_output_and_reports_them(self):
        log_path = self.state_dir / "marker.log"
        log_path.parent.mkdir(parents=True, exist_ok=True)
        seen = {}

        def report_progress(**fields):
            seen.update(fields)

        result = self.mod.run_logged_command(
            [
                "python3",
                "-c",
                (
                    "print('__PULP_VALIDATION__:smoke');"
                    "print('__PULP_TEST_POLICY__:skip');"
                    "print('__PULP_PHASE__:build');"
                    "print('done')"
                ),
            ],
            log_path=log_path,
            report_progress=report_progress,
        )

        self.assertEqual(result["returncode"], 0)
        self.assertIn("__PULP_VALIDATION__:smoke", result["output"])
        self.assertIn("__PULP_TEST_POLICY__:skip", result["output"])
        self.assertIn("__PULP_PHASE__:build", result["output"])
        self.assertIn("done", result["output"])
        logged = log_path.read_text()
        self.assertIn("__PULP_VALIDATION__:smoke", logged)
        self.assertIn("__PULP_TEST_POLICY__:skip", logged)
        self.assertEqual(seen["validation_mode"], "smoke")
        self.assertEqual(seen["test_policy"], "skip")
        self.assertEqual(seen["phase"], "build")

    def test_run_logged_command_emits_quiet_heartbeat_and_stuck_state(self):
        seen: list[dict] = []

        def report_progress(**fields):
            seen.append(dict(fields))

        result = self.mod.run_logged_command(
            [
                "python3",
                "-c",
                "import time; time.sleep(0.18); print('done')",
            ],
            report_progress=report_progress,
            heartbeat_interval_secs=0.05,
            stuck_idle_secs=0.1,
        )

        self.assertEqual(result["returncode"], 0)
        heartbeat_events = [item for item in seen if item.get("last_heartbeat_at")]
        self.assertTrue(heartbeat_events, msg=f"missing heartbeat events in {seen}")
        liveness_values = {item.get("liveness") for item in heartbeat_events}
        self.assertTrue(
            liveness_values <= {"quiet", "stuck"},
            msg=f"unexpected heartbeat states in {heartbeat_events}",
        )
        self.assertTrue(
            "stuck" in liveness_values,
            msg=f"missing stuck heartbeat in {heartbeat_events}",
        )
        self.assertTrue(any(item.get("last_output_at") for item in seen))

    def test_run_logged_command_replaces_invalid_utf8_bytes(self):
        log_path = self.state_dir / "nonutf8.log"
        log_path.parent.mkdir(parents=True, exist_ok=True)

        result = self.mod.run_logged_command(
            [
                "python3",
                "-c",
                (
                    "import sys; "
                    "sys.stdout.buffer.write(b'prefix\\xe5suffix\\\\n'); "
                    "sys.stdout.flush()"
                ),
            ],
            log_path=log_path,
        )

        self.assertEqual(result["returncode"], 0)
        self.assertIn("prefix", result["output"])
        self.assertIn("suffix", result["output"])
        self.assertIn("\ufffd", result["output"])
        logged = log_path.read_text()
        self.assertIn("prefix", logged)
        self.assertIn("suffix", logged)

    def test_target_log_path_uses_machine_global_logs_dir(self):
        path = self.mod.target_log_path("job123", "windows")
        self.assertEqual(path, self.state_dir / "logs" / "job123" / "windows.log")

    def test_save_result_updates_evidence_index_with_last_good_target_results(self):
        result_path_one = self.mod.save_result(
            {
                "job_id": "job111",
                "branch": "feature/evidence",
                "sha": "1" * 40,
                "priority": "normal",
                "validation": "full",
                "targets": ["mac", "ubuntu"],
                "queued_at": "2026-04-01T00:00:00+00:00",
                "completed_at": "2026-04-01T00:10:00+00:00",
                "results": [
                    {"target": "mac", "status": "pass", "duration_secs": 10.0},
                    {"target": "ubuntu", "status": "fail", "duration_secs": 20.0},
                ],
                "overall": "fail",
            }
        )
        self.assertTrue(result_path_one.exists())

        self.mod.save_result(
            {
                "job_id": "job112",
                "branch": "feature/evidence",
                "sha": "1" * 40,
                "priority": "normal",
                "validation": "full",
                "targets": ["ubuntu"],
                "queued_at": "2026-04-01T00:11:00+00:00",
                "completed_at": "2026-04-01T00:20:00+00:00",
                "results": [
                    {"target": "ubuntu", "status": "pass", "duration_secs": 12.0},
                ],
                "overall": "pass",
            }
        )

        index = self.mod.load_evidence_index()
        self.assertIn(
            self.mod.evidence_entry_key("feature/evidence", "1" * 40, "mac", "full"),
            index["entries"],
        )
        self.assertIn(
            self.mod.evidence_entry_key("feature/evidence", "1" * 40, "ubuntu", "full"),
            index["entries"],
        )
        self.assertEqual(
            index["entries"][
                self.mod.evidence_entry_key("feature/evidence", "1" * 40, "ubuntu", "full")
            ]["job_id"],
            "job112",
        )

    def test_branch_scoped_evidence_survives_same_sha_on_another_branch(self):
        shared_sha = "4" * 40
        self.mod.save_result(
            {
                "job_id": "job401",
                "branch": "feature/alpha",
                "sha": shared_sha,
                "priority": "normal",
                "validation": "full",
                "targets": ["mac"],
                "queued_at": "2026-04-01T03:00:00+00:00",
                "completed_at": "2026-04-01T03:10:00+00:00",
                "results": [
                    {"target": "mac", "status": "pass", "duration_secs": 8.0},
                ],
                "overall": "pass",
            }
        )
        self.mod.save_result(
            {
                "job_id": "job402",
                "branch": "main",
                "sha": shared_sha,
                "priority": "normal",
                "validation": "full",
                "targets": ["mac"],
                "queued_at": "2026-04-01T03:11:00+00:00",
                "completed_at": "2026-04-01T03:20:00+00:00",
                "results": [
                    {"target": "mac", "status": "pass", "duration_secs": 7.5},
                ],
                "overall": "pass",
            }
        )

        feature_groups = self.mod.collect_evidence_groups(branch="feature/alpha")
        self.assertEqual(len(feature_groups["full"]), 1)
        self.assertEqual(feature_groups["full"][0]["sha"], shared_sha)
        self.assertEqual(feature_groups["full"][0]["branch"], "feature/alpha")
        self.assertIn("mac", feature_groups["full"][0]["targets"])

    def test_cmd_evidence_prints_grouped_branch_summary(self):
        self.mod.save_result(
            {
                "job_id": "job201",
                "branch": "feature/evidence",
                "sha": "2" * 40,
                "priority": "normal",
                "validation": "smoke",
                "targets": ["mac", "windows"],
                "queued_at": "2026-04-01T01:00:00+00:00",
                "completed_at": "2026-04-01T01:10:00+00:00",
                "results": [
                    {"target": "mac", "status": "pass", "duration_secs": 9.0},
                    {"target": "windows", "status": "pass", "duration_secs": 15.0},
                ],
                "overall": "pass",
            }
        )

        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_evidence(
                SimpleNamespace(branch="feature/evidence", sha=None, limit=5)
            )

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Evidence for branch `feature/evidence`:", output)
        self.assertIn("smoke:", output)
        self.assertIn("mac=pass, windows=pass", output)
        self.assertIn("222222222222", output)

    def test_cmd_status_includes_current_branch_evidence_summary(self):
        self.mod.save_result(
            {
                "job_id": "job301",
                "branch": "feature/status-evidence",
                "sha": "3" * 40,
                "priority": "normal",
                "validation": "full",
                "targets": ["mac", "ubuntu", "windows"],
                "queued_at": "2026-04-01T02:00:00+00:00",
                "completed_at": "2026-04-01T02:30:00+00:00",
                "results": [
                    {"target": "mac", "status": "pass", "duration_secs": 10.0},
                    {"target": "ubuntu", "status": "pass", "duration_secs": 12.0},
                    {"target": "windows", "status": "pass", "duration_secs": 14.0},
                ],
                "overall": "pass",
            }
        )

        original_current_branch = self.mod.current_branch
        original_utm_status = self.mod.utmctl_vm_status
        original_ssh_reachable = self.mod.ssh_reachable
        self.mod.current_branch = lambda: "feature/status-evidence"
        self.mod.utmctl_vm_status = lambda vm_name: "stopped"
        self.mod.ssh_reachable = lambda host, timeout=5: True
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_status(SimpleNamespace())
        finally:
            self.mod.current_branch = original_current_branch
            self.mod.utmctl_vm_status = original_utm_status
            self.mod.ssh_reachable = original_ssh_reachable

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Evidence (feature/status-evidence):", output)
        self.assertIn("333333333333", output)
        self.assertIn("mac=pass, ubuntu=pass, windows=pass", output)

    def test_cmd_status_shows_heartbeat_idle_and_liveness(self):
        job, _created = self.mod.enqueue_job(
            "feature/observability",
            "4" * 40,
            "normal",
            ["windows"],
            "run",
            "full",
        )
        with self.mod.file_lock(self.mod.queue_lock_path(), blocking=True):
            queue = self.mod.load_queue_unlocked()
            stored = self.mod.find_job_unlocked(queue, job["id"])
            self.assertIsNotNone(stored)
            stored["status"] = "running"
            stored["started_at"] = "2026-04-02T05:00:00+00:00"
            stored["runner"] = {"pid": os.getpid(), "root": str(self.state_dir)}
            stored["active_targets"] = {
                "windows": {
                    "status": "running",
                    "phase": "build",
                    "last_output_at": "2026-04-02T05:00:10+00:00",
                    "last_heartbeat_at": "2026-04-02T05:01:10+00:00",
                    "quiet_for_secs": 60,
                    "liveness": "stuck",
                    "log_path": str(self.state_dir / "logs" / "job.log"),
                }
            }
            self.mod.save_queue_unlocked(queue)
            self.mod.write_runner_info(
                {
                    "pid": os.getpid(),
                    "root": str(self.state_dir),
                    "active_job_id": job["id"],
                    "active_branch": job["branch"],
                    "active_targets": stored["active_targets"],
                }
            )

        original_current_branch = self.mod.current_branch
        original_utm_status = self.mod.utmctl_vm_status
        original_ssh_reachable = self.mod.ssh_reachable
        self.mod.current_branch = lambda: "feature/observability"
        self.mod.utmctl_vm_status = lambda vm_name: "stopped"
        self.mod.ssh_reachable = lambda host, timeout=5: True
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_status(SimpleNamespace())
        finally:
            self.mod.current_branch = original_current_branch
            self.mod.utmctl_vm_status = original_utm_status
            self.mod.ssh_reachable = original_ssh_reachable

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("heartbeat=2026-04-02T05:01:10+00:00", output)
        self.assertIn("idle=60s", output)
        self.assertIn("liveness=stuck", output)

    def test_build_submission_metadata_adds_default_provenance(self):
        config = self.mod.load_config()
        metadata = self.mod.build_submission_metadata(
            config,
            "feature/provenance",
            "a" * 40,
            ["mac"],
            "normal",
            "full",
            allow_root_mismatch=True,
            allow_unreachable_targets=False,
        )

        self.assertEqual(metadata["provenance"]["execution_kind"], "direct")
        self.assertEqual(metadata["provenance"]["control_plane"], "pulp-ci-local")
        self.assertEqual(metadata["provenance"]["direct_backend"], "local-ci")
        self.assertEqual(metadata["provenance"]["hosted_orchestrator"], "")

    def test_load_result_backfills_default_provenance(self):
        result_path = self.state_dir / "legacy-result.json"
        result_path.parent.mkdir(parents=True, exist_ok=True)
        result_path.write_text(
            json.dumps(
                {
                    "job_id": "job123",
                    "branch": "feature/legacy",
                    "sha": "b" * 40,
                    "results": [],
                    "overall": "pass",
                }
            )
        )

        result = self.mod.load_result(result_path)
        self.assertEqual(result["provenance"]["execution_kind"], "direct")
        self.assertEqual(result["provenance"]["direct_backend"], "local-ci")

    def test_evidence_record_carries_provenance(self):
        result = {
            "job_id": "job123",
            "branch": "feature/evidence",
            "sha": "c" * 40,
            "validation": "full",
            "completed_at": "2026-04-04T12:00:00+00:00",
            "provenance": {
                "execution_kind": "hosted",
                "control_plane": "pulp-ci-local",
                "direct_backend": "",
                "hosted_orchestrator": "github-actions",
                "runner_provider": "namespace",
                "runner_selector": "mac-arm64",
                "run_id": "12345",
                "run_url": "https://example.test/runs/12345",
            },
        }
        item = {"target": "mac", "status": "pass", "duration_secs": 12}

        record = self.mod.evidence_record_from_result(result, item, self.state_dir / "result.json")
        self.assertEqual(record["provenance"]["hosted_orchestrator"], "github-actions")
        self.assertEqual(record["provenance"]["runner_provider"], "namespace")
        self.assertEqual(record["provenance"]["runner_selector"], "mac-arm64")

    def test_format_ci_comment_includes_execution_summary(self):
        comment = self.mod.format_ci_comment(
            {
                "job_id": "job123",
                "branch": "feature/comment",
                "sha": "d" * 40,
                "completed_at": "2026-04-04T12:00:00+00:00",
                "overall": "pass",
                "results": [{"target": "mac", "status": "pass", "duration_secs": 12}],
            }
        )

        self.assertIn("Execution: `direct via local-ci`", comment)

    def test_resolve_github_actions_settings_reads_optional_config_defaults(self):
        settings = self.mod.resolve_github_actions_settings(self.mod.load_optional_config())
        self.assertEqual(settings["repository"], "danielraffel/pulp")
        self.assertEqual(settings["workflow"], "build")
        self.assertEqual(settings["provider"], "github-hosted")
        self.assertEqual(settings["wait_poll_secs"], 5)
        self.assertEqual(settings["match_timeout_secs"], 30)

    def test_resolve_workflow_runner_selector_json_reads_docs_check_provider_default(self):
        selector = self.mod.resolve_workflow_runner_selector_json(
            self.mod.load_optional_config(), "docs-check", "namespace"
        )
        self.assertEqual(selector, "\"namespace-profile-default\"")

    def test_resolve_default_provider_for_workflow_falls_back_to_github_hosted(self):
        provider, source = self.mod.resolve_default_provider_for_workflow(
            {"provider": "namespace"},
            "validate",
        )
        self.assertEqual(provider, "github-hosted")
        self.assertIn("workflow fallback", source)

    def test_resolve_workflow_field_value_and_source_reads_repo_variable_fallback(self):
        config = json.loads(self.config_path.read_text())
        del config["github_actions"]["workflows"]["docs-check"]["providers"]["namespace"]["runner_selector_json"]
        self.config_path.write_text(json.dumps(config) + "\n")

        value, source = self.mod.resolve_workflow_field_value_and_source(
            self.mod.load_optional_config(),
            {"PULP_NAMESPACE_DOCS_CHECK_RUNS_ON_JSON": "\"namespace-profile-repo-var\""},
            "docs-check",
            "namespace",
            "runner_selector_json",
        )
        self.assertEqual(value, "\"namespace-profile-repo-var\"")
        self.assertEqual(source, "repo variable PULP_NAMESPACE_DOCS_CHECK_RUNS_ON_JSON")

    def test_resolve_workflow_dispatch_field_values_reads_build_namespace_defaults(self):
        fields = self.mod.resolve_workflow_dispatch_field_values(
            self.mod.load_optional_config(),
            "build",
            "namespace",
            ["linux_runner_selector_json", "windows_runner_selector_json"],
        )
        self.assertEqual(
            fields,
            {
                "linux_runner_selector_json": "\"namespace-profile-default\"",
                "windows_runner_selector_json": "\"namespace-profile-default\"",
            },
        )

    def test_cloud_record_round_trip_and_lookup(self):
        first = self.mod.normalize_cloud_record(
            {
                "dispatch_id": "abc123def456",
                "workflow_key": "docs-check",
                "requested_ref": "feature/docs",
                "provider_requested": "namespace",
                "status": "in_progress",
                "dispatched_at": "2026-04-04T12:00:00+00:00",
                "updated_at": "2026-04-04T12:01:00+00:00",
            }
        )
        second = self.mod.normalize_cloud_record(
            {
                "dispatch_id": "fed654cba321",
                "workflow_key": "build",
                "requested_ref": "feature/build",
                "provider_requested": "github-hosted",
                "status": "completed",
                "conclusion": "success",
                "run_id": 12345,
                "dispatched_at": "2026-04-04T12:02:00+00:00",
                "updated_at": "2026-04-04T12:03:00+00:00",
            }
        )

        self.mod.save_cloud_record(first)
        self.mod.save_cloud_record(second)

        records = self.mod.list_cloud_records()
        self.assertEqual(records[0]["dispatch_id"], "fed654cba321")
        self.assertEqual(self.mod.find_cloud_record(records, "latest")["dispatch_id"], "fed654cba321")
        self.assertEqual(self.mod.find_cloud_record(records, "abc123")["dispatch_id"], "abc123def456")
        self.assertEqual(self.mod.find_cloud_record(records, "12345")["dispatch_id"], "fed654cba321")

    def test_update_cloud_record_from_run_derives_timing(self):
        record = self.mod.normalize_cloud_record(
            {
                "dispatch_id": "abc123def456",
                "workflow_key": "docs-check",
                "requested_ref": "feature/docs",
                "provider_requested": "namespace",
                "dispatched_at": "2026-04-04T12:00:00+00:00",
            }
        )
        snapshot = {
            "databaseId": 98765,
            "workflowName": "Docs Consistency",
            "headBranch": "feature/docs",
            "headSha": "a" * 40,
            "status": "completed",
            "conclusion": "success",
            "url": "https://example.test/runs/98765",
            "createdAt": "2026-04-04T12:00:05+00:00",
            "updatedAt": "2026-04-04T12:00:30+00:00",
            "jobs": [
                {
                    "name": "Resolve provider",
                    "status": "completed",
                    "conclusion": "success",
                    "startedAt": "2026-04-04T12:00:06+00:00",
                    "completedAt": "2026-04-04T12:00:08+00:00",
                },
                {
                    "name": "Validate docs consistency",
                    "status": "completed",
                    "conclusion": "success",
                    "startedAt": "2026-04-04T12:00:09+00:00",
                    "completedAt": "2026-04-04T12:00:30+00:00",
                },
            ],
        }

        updated = self.mod.update_cloud_record_from_run(record, snapshot, provider_resolved="namespace")

        self.assertEqual(updated["started_at"], "2026-04-04T12:00:06+00:00")
        self.assertEqual(updated["completed_at"], "2026-04-04T12:00:30+00:00")
        self.assertEqual(updated["queue_delay_secs"], 1.0)
        self.assertEqual(updated["duration_secs"], 24.0)

    def test_update_cloud_record_from_run_clears_completed_at_when_still_running(self):
        record = self.mod.normalize_cloud_record(
            {
                "dispatch_id": "abc123def456",
                "workflow_key": "build",
                "requested_ref": "feature/build",
                "provider_requested": "namespace",
                "completed_at": "2026-04-04T12:00:30+00:00",
            }
        )
        snapshot = {
            "databaseId": 98765,
            "workflowName": "Build and Test",
            "headBranch": "feature/build",
            "headSha": "a" * 40,
            "status": "in_progress",
            "conclusion": "",
            "url": "https://example.test/runs/98765",
            "createdAt": "2026-04-04T12:00:05+00:00",
            "updatedAt": "2026-04-04T12:00:06+00:00",
            "jobs": [
                {
                    "name": "resolve-provider",
                    "status": "completed",
                    "conclusion": "success",
                    "startedAt": "2026-04-04T12:00:10+00:00",
                    "completedAt": "2026-04-04T12:00:12+00:00",
                },
                {
                    "name": "Linux (x64) [namespace]",
                    "status": "in_progress",
                    "conclusion": "",
                    "startedAt": "2026-04-04T12:00:15+00:00",
                    "completedAt": "0001-01-01T00:00:00Z",
                    "steps": [
                        {
                            "startedAt": "2026-04-04T12:00:20+00:00",
                            "completedAt": "0001-01-01T00:00:00Z",
                        }
                    ],
                },
            ],
        }

        updated = self.mod.update_cloud_record_from_run(record, snapshot, provider_resolved="namespace")

        self.assertEqual(updated["completed_at"], "")
        self.assertEqual(updated["duration_secs"], 10.0)

    def test_cloud_record_summary_includes_duration(self):
        summary = self.mod.cloud_record_summary(
            {
                "dispatch_id": "abc123def456",
                "workflow_key": "docs-check",
                "requested_ref": "feature/docs",
                "provider_requested": "namespace",
                "status": "completed",
                "conclusion": "success",
                "run_id": 98765,
                "duration_secs": 84,
            }
        )
        self.assertIn("duration=1m24s", summary)

    def test_summarize_cloud_timing_uses_latest_step_timestamp_for_in_progress_run(self):
        timing = self.mod.summarize_cloud_timing(
            {
                "status": "in_progress",
                "createdAt": "2026-04-04T12:00:05+00:00",
                "updatedAt": "2026-04-04T12:00:06+00:00",
                "jobs": [
                    {
                        "startedAt": "2026-04-04T12:00:10+00:00",
                        "completedAt": "0001-01-01T00:00:00Z",
                        "steps": [
                            {
                                "startedAt": "2026-04-04T12:00:25+00:00",
                                "completedAt": "0001-01-01T00:00:00Z",
                            }
                        ],
                    }
                ],
            }
        )

        self.assertEqual(timing["started_at"], "2026-04-04T12:00:10+00:00")
        self.assertEqual(timing["completed_at"], "")
        self.assertEqual(timing["duration_secs"], 15.0)

    def test_parse_and_normalize_helpers_cover_invalid_edges(self):
        self.assertTrue(self.mod.parse_config_bool(" On "))
        self.assertTrue(self.mod.parse_config_bool(2))
        self.assertFalse(self.mod.parse_config_bool(" no "))
        self.assertFalse(self.mod.parse_config_bool(0))
        with self.assertRaisesRegex(ValueError, "Invalid boolean value"):
            self.mod.parse_config_bool("sometimes")

        self.assertEqual(self.mod.normalize_validation_mode(None), "full")
        self.assertEqual(self.mod.normalize_validation_mode(" SMOKE "), "smoke")
        with self.assertRaisesRegex(ValueError, "Invalid validation mode"):
            self.mod.normalize_validation_mode("quick")

        self.assertEqual(self.mod.normalize_desktop_source_mode("exact_sha"), "exact-sha")
        with self.assertRaisesRegex(ValueError, "Invalid desktop source mode"):
            self.mod.normalize_desktop_source_mode("archive")

        self.assertEqual(self.mod.normalize_publish_mode(" ISSUE-COMMENT "), "issue-comment")
        with self.assertRaisesRegex(ValueError, "Invalid desktop publish mode"):
            self.mod.normalize_publish_mode("rss")

        self.assertEqual(
            self.mod.normalize_runs_on_json('["self-hosted", "macOS"]', setting_name="runs-on"),
            '["self-hosted", "macOS"]',
        )
        self.assertEqual(
            self.mod.normalize_runs_on_json('"ubuntu-latest"', setting_name="runs-on"),
            '"ubuntu-latest"',
        )
        with self.assertRaisesRegex(ValueError, "valid JSON"):
            self.mod.normalize_runs_on_json("ubuntu-latest", setting_name="runs-on")
        with self.assertRaisesRegex(ValueError, "string or array"):
            self.mod.normalize_runs_on_json('{"label": "ubuntu"}', setting_name="runs-on")

    def test_workflow_selector_helpers_resolve_config_repo_and_cli_values(self):
        config = {
            "github_actions": {
                "defaults": {"provider": "namespace"},
                "workflows": {
                    "build": {
                        "providers": {
                            "namespace": {
                                "linux_runner_selector_json": '"linux-config"',
                            }
                        }
                    },
                    "docs-check": {
                        "providers": {
                            "namespace": {},
                        }
                    },
                },
            }
        }
        repo_variables = {
            "PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON": '"windows-repo"',
            "PULP_NAMESPACE_BUILD_MACOS_RUNS_ON_JSON": '["self-hosted", "macOS"]',
            "PULP_NAMESPACE_DOCS_CHECK_RUNS_ON_JSON": '"docs-repo"',
        }
        settings = self.mod.resolve_github_actions_settings(config)

        build_summary = self.mod.summarize_workflow_provider_defaults(
            config, repo_variables, settings, "build"
        )
        self.assertEqual(build_summary["provider"], "namespace")
        self.assertEqual(
            build_summary["dispatch_fields"],
            {
                "linux_runner_selector_json": '"linux-config"',
                "windows_runner_selector_json": '"windows-repo"',
                "macos_runner_selector_json": '["self-hosted", "macOS"]',
            },
        )
        self.assertEqual(
            build_summary["dispatch_sources"]["linux_runner_selector_json"],
            "config github_actions.workflows.build.providers.namespace.linux_runner_selector_json",
        )
        self.assertEqual(
            build_summary["dispatch_sources"]["windows_runner_selector_json"],
            "repo variable PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON",
        )

        docs_summary = self.mod.summarize_workflow_provider_defaults(
            config, repo_variables, settings, "docs-check"
        )
        self.assertEqual(docs_summary["selector_value"], '"docs-repo"')
        self.assertEqual(
            docs_summary["selector_source"],
            "repo variable PULP_NAMESPACE_DOCS_CHECK_RUNS_ON_JSON",
        )

        cli_fields = self.mod.resolve_cli_dispatch_field_values(
            SimpleNamespace(
                linux_runner_selector_json='"linux-cli"',
                windows_runner_selector_json=None,
                macos_runner_selector_json=None,
            ),
            ["linux_runner_selector_json"],
        )
        self.assertEqual(cli_fields, {"linux_runner_selector_json": '"linux-cli"'})
        with self.assertRaisesRegex(ValueError, "not supported"):
            self.mod.resolve_cli_dispatch_field_values(
                SimpleNamespace(
                    linux_runner_selector_json='"linux-cli"',
                    windows_runner_selector_json=None,
                    macos_runner_selector_json=None,
                ),
                [],
            )
        with self.assertRaisesRegex(ValueError, "valid JSON"):
            self.mod.resolve_cli_dispatch_field_values(
                SimpleNamespace(
                    linux_runner_selector_json="linux-cli",
                    windows_runner_selector_json=None,
                    macos_runner_selector_json=None,
                ),
                ["linux_runner_selector_json"],
            )

    def test_cloud_record_lookup_and_summary_helpers_cover_edge_statuses(self):
        normalized = self.mod.normalize_cloud_record(
            {
                "dispatch_fields": [],
                "jobs": {},
                "provider_metadata": [],
                "usage_summary": [],
                "cost_summary": [],
            }
        )
        self.assertEqual(normalized["dispatch_fields"], {})
        self.assertEqual(normalized["jobs"], [])
        self.assertEqual(normalized["provider_metadata"], {})
        self.assertEqual(normalized["usage_summary"], {})
        self.assertEqual(normalized["cost_summary"], {})

        records = [
            {"dispatch_id": "abc111", "run_id": 42},
            {"dispatch_id": "abc222", "run_id": 42},
            {"dispatch_id": "def333", "run_id": 99},
        ]
        self.assertIsNone(self.mod.find_cloud_record([], None))
        self.assertIsNone(self.mod.find_cloud_record(records, "missing"))
        with self.assertRaisesRegex(ValueError, "ambiguous"):
            self.mod.find_cloud_record(records, "abc")
        with self.assertRaisesRegex(ValueError, "matched multiple"):
            self.mod.find_cloud_record(records, "42")

        config = {
            "telemetry": {
                "billing": {
                    "namespace_machine_shape_rates_per_hour": [
                        {
                            "os": "linux",
                            "arch": "amd64",
                            "virtual_cpu": 4,
                            "memory_megabytes": 8192,
                            "rate": 1.25,
                        }
                    ]
                }
            }
        }
        summary = self.mod.cloud_record_summary(
            {
                "dispatch_id": "sum123",
                "workflow_key": "build",
                "head_branch": "feature/cloud",
                "provider_requested": "namespace",
                "provider_resolved": "namespace",
                "status": "completed",
                "conclusion": "success",
                "run_id": 123,
                "runner_selector_json": '["self-hosted", "linux"]',
                "duration_secs": 65.4,
                "usage_summary": {"provider_runtime_secs": 3600},
                "provider_metadata": {
                    "namespace_instances": [
                        {
                            "os": "linux",
                            "arch": "amd64",
                            "virtual_cpu": 4,
                            "memory_megabytes": 8192,
                            "duration_secs": 7200,
                        }
                    ]
                },
            },
            config,
        )
        self.assertIn("selector=self-hosted,linux", summary)
        self.assertIn("gha#123", summary)
        self.assertIn("duration=1m05s", summary)
        self.assertIn("provider_time=1h00m00s", summary)
        self.assertIn("cost=est $2.50", summary)

        self.assertEqual(self.mod.summarize_runner_selector("{not-json"), "{not-json")
        self.assertEqual(self.mod.summarize_runner_selector("123"), "123")
        self.assertEqual(self.mod.normalize_github_timestamp("0001-01-01T00:00:00Z"), "")
        self.assertIsNone(self.mod.duration_between("bad", "2026-04-04T12:00:00+00:00"))
        self.assertEqual(
            self.mod.duration_between(
                "2026-04-04T12:00:10+00:00",
                "2026-04-04T12:00:05+00:00",
            ),
            0.0,
        )

    def test_billing_estimation_and_report_helpers_cover_fallbacks(self):
        self.assertEqual(self.mod.format_duration_secs(None), "")
        self.assertEqual(self.mod.format_duration_secs("bad"), "")
        self.assertEqual(self.mod.format_duration_secs(-1), "")
        self.assertEqual(self.mod.format_duration_secs(1.25), "1.2s")
        self.assertEqual(self.mod.format_duration_secs(3661), "1h01m01s")

        self.assertEqual(self.mod.format_memory_megabytes("bad"), "")
        self.assertEqual(self.mod.format_memory_megabytes(0), "")
        self.assertEqual(self.mod.format_memory_megabytes(1536), "1.5 GB")
        self.assertEqual(self.mod.render_selector_value('"macos"'), "macos")
        self.assertIsNone(self.mod.parse_rate_value("-1"))
        self.assertIsNone(self.mod.parse_rate_value("bad"))
        self.assertTrue(self.mod.parse_optional_bool(True, "enabled"))
        self.assertIsNone(self.mod.parse_optional_bool("", "enabled"))
        with self.assertRaisesRegex(ValueError, "must be true or false"):
            self.mod.parse_optional_bool("yes", "enabled")

        config = {
            "telemetry": {
                "billing": {
                    "currency": "eur",
                    "billing_period_start_day": "15",
                    "enable_provider_reported_totals": True,
                    "github_hosted_job_os_rates_per_minute": {
                        " Linux ": "0.02",
                        "": "9.99",
                        "macos": "-1",
                    },
                    "namespace_profile_tag_rates_per_hour": {
                        "fast": "1.5",
                        "": "9.99",
                        "slow": "bad",
                    },
                    "namespace_machine_shape_rates_per_hour": [
                        {
                            "os": "Linux",
                            "arch": "AMD64",
                            "virtual_cpu": "8",
                            "memory_megabytes": "16384",
                            "rate": "2.0",
                        },
                        "ignored",
                        {"rate": "-1"},
                    ],
                }
            }
        }
        billing = self.mod.resolve_billing_settings(config)
        self.assertEqual(billing["currency"], "EUR")
        self.assertEqual(billing["billing_period_start_day"], 15)
        self.assertTrue(billing["enable_provider_reported_totals"])
        self.assertEqual(billing["github_hosted_job_os_rates_per_minute"], {"linux": 0.02})
        self.assertEqual(billing["namespace_profile_tag_rates_per_hour"], {"fast": 1.5})
        self.assertEqual(
            billing["namespace_machine_shape_rates_per_hour"],
            [
                {
                    "os": "linux",
                    "arch": "amd64",
                    "virtual_cpu": 8,
                    "memory_megabytes": 16384,
                    "rate": 2.0,
                }
            ],
        )
        with self.assertRaisesRegex(ValueError, "must be between 1 and 28"):
            self.mod.resolve_billing_settings(
                {"telemetry": {"billing": {"billing_period_start_day": 31}}}
            )
        with self.assertRaisesRegex(ValueError, "must be true or false"):
            self.mod.resolve_billing_settings(
                {"telemetry": {"billing": {"enable_provider_reported_totals": "yes"}}}
            )

        period_start, period_end = self.mod.billing_period_window(
            15, now_dt=datetime(2026, 1, 10, tzinfo=timezone.utc)
        )
        self.assertEqual(period_start.isoformat(), "2025-12-15T00:00:00+00:00")
        self.assertEqual(period_end.isoformat(), "2026-01-15T00:00:00+00:00")
        self.assertEqual(
            self.mod.iter_year_months(
                datetime(2025, 12, 15, tzinfo=timezone.utc),
                datetime(2026, 2, 15, tzinfo=timezone.utc),
            ),
            [(2025, 12), (2026, 1), (2026, 2)],
        )
        self.assertIsNone(self.mod.parse_iso_date(""))
        self.assertIsNone(self.mod.parse_iso_date("2026-99-99"))
        self.assertEqual(self.mod.parse_iso_date("2026-04-04").isoformat(), "2026-04-04")
        self.assertEqual(self.mod.infer_job_os("build", "Windows (x64)"), "windows")
        self.assertEqual(self.mod.infer_job_os("build", "macOS (ARM64)"), "macos")
        self.assertEqual(self.mod.infer_job_os("build", "Ubuntu tests"), "linux")
        self.assertEqual(self.mod.infer_job_os("docs-check", "Validate docs"), "linux")
        self.assertEqual(self.mod.infer_job_os("build", "Resolve provider"), "")

        github_cost = self.mod.estimate_github_hosted_cost(
            {
                "workflow_key": "build",
                "jobs": [
                    {"name": "resolve-provider"},
                    {
                        "name": "Linux (x64)",
                        "started_at": "2026-04-04T12:00:00+00:00",
                        "completed_at": "2026-04-04T12:02:00+00:00",
                    },
                    {"name": "Unknown job"},
                ],
            },
            billing,
        )
        self.assertEqual(github_cost["status"], "estimated")
        self.assertAlmostEqual(github_cost["estimated_total"], 0.04)

        namespace_cost = self.mod.estimate_namespace_cost(
            {
                "usage_summary": {
                    "machine_shapes": [
                        {
                            "os": "linux",
                            "arch": "amd64",
                            "virtual_cpu": 8,
                            "memory_megabytes": 16384,
                            "duration_secs": 1800,
                        }
                    ]
                }
            },
            billing,
        )
        self.assertEqual(namespace_cost["status"], "estimated")
        self.assertAlmostEqual(namespace_cost["estimated_total"], 1.0)
        self.assertEqual(
            self.mod.estimate_cloud_record_cost({"provider_requested": "other"}, config)["reason"],
            "no estimator for provider 'other'",
        )
        self.assertEqual(self.mod.format_currency_amount(3.5, "eur"), "EUR 3.50")

        buf = io.StringIO()
        with redirect_stdout(buf):
            self.mod.print_github_repo_billing_summary(
                {"status": "unavailable", "reason": "missing scope"}, indent=""
            )
            self.mod.print_cloud_field_detail(
                "runner_selector_json", '["self-hosted", "linux"]', "config", indent=""
            )
            self.mod.print_cloud_field_detail(
                "plain", "", indent="", unset_note="missing"
            )
            self.mod.print_namespace_usage_summary(
                {
                    "usage_summary": {
                        "instances_count": 1,
                        "provider_runtime_secs": 0,
                        "machine_shapes": [
                            {
                                "profile_tag": "",
                                "count": 1,
                                "duration_secs": 0,
                            }
                        ],
                    },
                    "cost_summary": {
                        "status": "estimated",
                        "estimated_total": 1.2,
                        "currency": "EUR",
                        "reason": "",
                    },
                }
            )
            self.mod.print_billing_period_summary(
                {"status": "unavailable", "reason": "no rates"}, indent=""
            )

        output = buf.getvalue()
        self.assertIn("github repo billing: unavailable (missing scope)", output)
        self.assertIn("runner_selector_json: self-hosted,linux (config)", output)
        self.assertIn("plain: unset (missing)", output)
        self.assertIn("provider usage: 1 Namespace instance(s)", output)
        self.assertIn("unlabeled: unknown x1", output)
        self.assertIn("cost: est EUR 1.20; estimated; verify provider pricing", output)
        self.assertIn("period cost: unavailable (no rates)", output)

    def test_cmd_cloud_workflows_lists_supported_providers(self):
        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_cloud_workflows(SimpleNamespace())

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("build", output)
        self.assertIn("docs-check", output)
        self.assertIn("namespace", output)

    def test_cmd_cloud_status_reports_empty_state_cleanly(self):
        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_cloud_status(SimpleNamespace(identifier=None, refresh=False, limit=5))

        self.assertEqual(exit_code, 0)
        self.assertIn("No tracked cloud runs yet.", buf.getvalue())

    def test_cmd_cloud_namespace_doctor_reports_missing_cli(self):
        original_version = self.mod.nsc_version
        self.mod.nsc_version = lambda: None
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_namespace_doctor(SimpleNamespace())
        finally:
            self.mod.nsc_version = original_version

        self.assertEqual(exit_code, 1)
        output = buf.getvalue()
        self.assertIn("Namespace CLI: missing", output)
        self.assertIn("nsc login", output)

    def test_cmd_cloud_namespace_doctor_reports_ready_workspace(self):
        original_version = self.mod.nsc_version
        original_logged_in = self.mod.nsc_logged_in
        original_workspace_info = self.mod.nsc_workspace_info
        self.mod.nsc_version = lambda: "v0.0.493"
        self.mod.nsc_logged_in = lambda: True
        self.mod.nsc_workspace_info = lambda: {
            "Name": "Personal",
            "Tenant ID": "tenant_123",
            "Registry URL": "nscr.io/example",
        }
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_namespace_doctor(SimpleNamespace())
        finally:
            self.mod.nsc_version = original_version
            self.mod.nsc_logged_in = original_logged_in
            self.mod.nsc_workspace_info = original_workspace_info

        self.assertEqual(exit_code, 0)
        output = buf.getvalue()
        self.assertIn("Namespace CLI: ok (v0.0.493)", output)
        self.assertIn("Namespace login: ok", output)
        self.assertIn("Workspace: Personal", output)
        self.assertIn("Tenant ID: tenant_123", output)
        self.assertIn("Registry URL: nscr.io/example", output)

    def test_cmd_cloud_namespace_setup_invokes_login_then_reports_ready(self):
        original_available = self.mod.nsc_available
        original_logged_in = self.mod.nsc_logged_in
        original_run = self.mod.nsc_run
        original_version = self.mod.nsc_version
        original_workspace_info = self.mod.nsc_workspace_info

        login_checks = iter([False, True])
        calls = []
        self.mod.nsc_available = lambda: True
        self.mod.nsc_logged_in = lambda: next(login_checks)
        self.mod.nsc_run = lambda args, capture_output=True: calls.append((tuple(args), capture_output)) or SimpleNamespace(returncode=0)
        self.mod.nsc_version = lambda: "v0.0.493"
        self.mod.nsc_workspace_info = lambda: {"Name": "Personal"}
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_namespace_setup(SimpleNamespace())
        finally:
            self.mod.nsc_available = original_available
            self.mod.nsc_logged_in = original_logged_in
            self.mod.nsc_run = original_run
            self.mod.nsc_version = original_version
            self.mod.nsc_workspace_info = original_workspace_info

        self.assertEqual(exit_code, 0)
        self.assertEqual(calls, [(("login",), False)])
        output = buf.getvalue()
        self.assertIn("Namespace login: starting `nsc login`...", output)
        self.assertIn("Workspace: Personal", output)

    def test_cmd_cloud_run_rejects_unsupported_provider(self):
        original_gh_available = self.mod.gh_available
        self.mod.gh_available = lambda: True
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="validate",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json=None,
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json=None,
                        wait=False,
                    )
                )
        finally:
            self.mod.gh_available = original_gh_available

        output = buf.getvalue()
        self.assertEqual(exit_code, 1)
        self.assertIn("does not support provider", output)

    def test_cmd_cloud_run_build_namespace_dispatches_selector_fields(self):
        original_gh_available = self.mod.gh_available
        original_resolve_repo = self.mod.resolve_github_repository
        original_current_login = self.mod.gh_current_login
        original_dispatch = self.mod.gh_workflow_dispatch
        original_find = self.mod.gh_find_dispatched_run
        original_now_iso = self.mod.now_iso
        original_repo_variables = self.mod.gh_repo_variables

        self.mod.gh_available = lambda: True
        self.mod.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.mod.gh_current_login = lambda: "danielraffel"
        self.mod.gh_repo_variables = lambda repository: {}
        dispatched = {}
        self.mod.gh_workflow_dispatch = (
            lambda repository, workflow_file, ref, fields: dispatched.update(
                {
                    "repository": repository,
                    "workflow_file": workflow_file,
                    "ref": ref,
                    "fields": dict(fields),
                }
            )
        )
        self.mod.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: None
        self.mod.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="build",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json=None,
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json=None,
                        wait=False,
                    )
                )
        finally:
            self.mod.gh_available = original_gh_available
            self.mod.resolve_github_repository = original_resolve_repo
            self.mod.gh_current_login = original_current_login
            self.mod.gh_workflow_dispatch = original_dispatch
            self.mod.gh_find_dispatched_run = original_find
            self.mod.now_iso = original_now_iso
            self.mod.gh_repo_variables = original_repo_variables

        self.assertEqual(exit_code, 0)
        self.assertEqual(
            dispatched["fields"],
            {
                "runner_provider": "namespace",
                "linux_runner_selector_json": "\"namespace-profile-default\"",
                "windows_runner_selector_json": "\"namespace-profile-default\"",
            },
        )
        records = self.mod.list_cloud_records()
        self.assertEqual(records[0]["dispatch_fields"]["linux_runner_selector_json"], "\"namespace-profile-default\"")
        self.assertEqual(records[0]["dispatch_fields"]["windows_runner_selector_json"], "\"namespace-profile-default\"")

    def test_cmd_cloud_run_build_namespace_uses_repo_variable_selector_defaults(self):
        config = json.loads(self.config_path.read_text())
        del config["github_actions"]["workflows"]["build"]["providers"]["namespace"]["linux_runner_selector_json"]
        del config["github_actions"]["workflows"]["build"]["providers"]["namespace"]["windows_runner_selector_json"]
        self.config_path.write_text(json.dumps(config) + "\n")

        original_gh_available = self.mod.gh_available
        original_resolve_repo = self.mod.resolve_github_repository
        original_current_login = self.mod.gh_current_login
        original_dispatch = self.mod.gh_workflow_dispatch
        original_find = self.mod.gh_find_dispatched_run
        original_now_iso = self.mod.now_iso
        original_repo_variables = self.mod.gh_repo_variables

        self.mod.gh_available = lambda: True
        self.mod.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.mod.gh_current_login = lambda: "danielraffel"
        self.mod.gh_repo_variables = lambda repository: {
            "PULP_NAMESPACE_BUILD_LINUX_RUNS_ON_JSON": "\"namespace-profile-linux-repo\"",
            "PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON": "\"namespace-profile-windows-repo\"",
        }
        dispatched = {}
        self.mod.gh_workflow_dispatch = (
            lambda repository, workflow_file, ref, fields: dispatched.update(
                {
                    "repository": repository,
                    "workflow_file": workflow_file,
                    "ref": ref,
                    "fields": dict(fields),
                }
            )
        )
        self.mod.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: None
        self.mod.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="build",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json=None,
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json=None,
                        wait=False,
                    )
                )
        finally:
            self.mod.gh_available = original_gh_available
            self.mod.resolve_github_repository = original_resolve_repo
            self.mod.gh_current_login = original_current_login
            self.mod.gh_workflow_dispatch = original_dispatch
            self.mod.gh_find_dispatched_run = original_find
            self.mod.now_iso = original_now_iso
            self.mod.gh_repo_variables = original_repo_variables

        self.assertEqual(exit_code, 0)
        self.assertEqual(
            dispatched["fields"],
            {
                "runner_provider": "namespace",
                "linux_runner_selector_json": "\"namespace-profile-linux-repo\"",
                "windows_runner_selector_json": "\"namespace-profile-windows-repo\"",
            },
        )

    def test_cmd_cloud_run_build_namespace_includes_optional_macos_selector_when_present(self):
        config = json.loads(self.config_path.read_text())
        config["github_actions"]["workflows"]["build"]["providers"]["namespace"][
            "macos_runner_selector_json"
        ] = "\"namespace-profile-macos\""
        self.config_path.write_text(json.dumps(config) + "\n")

        original_gh_available = self.mod.gh_available
        original_resolve_repo = self.mod.resolve_github_repository
        original_current_login = self.mod.gh_current_login
        original_dispatch = self.mod.gh_workflow_dispatch
        original_find = self.mod.gh_find_dispatched_run
        original_now_iso = self.mod.now_iso

        self.mod.gh_available = lambda: True
        self.mod.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.mod.gh_current_login = lambda: "danielraffel"
        dispatched = {}
        self.mod.gh_workflow_dispatch = (
            lambda repository, workflow_file, ref, fields: dispatched.update(
                {
                    "repository": repository,
                    "workflow_file": workflow_file,
                    "ref": ref,
                    "fields": dict(fields),
                }
            )
        )
        self.mod.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: None
        self.mod.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="build",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json=None,
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json=None,
                        wait=False,
                    )
                )
        finally:
            self.mod.gh_available = original_gh_available
            self.mod.resolve_github_repository = original_resolve_repo
            self.mod.gh_current_login = original_current_login
            self.mod.gh_workflow_dispatch = original_dispatch
            self.mod.gh_find_dispatched_run = original_find
            self.mod.now_iso = original_now_iso

        self.assertEqual(exit_code, 0)
        self.assertEqual(
            dispatched["fields"]["macos_runner_selector_json"],
            "\"namespace-profile-macos\"",
        )

    def test_cmd_cloud_run_build_cli_override_adds_one_off_macos_selector(self):
        original_gh_available = self.mod.gh_available
        original_resolve_repo = self.mod.resolve_github_repository
        original_current_login = self.mod.gh_current_login
        original_dispatch = self.mod.gh_workflow_dispatch
        original_find = self.mod.gh_find_dispatched_run
        original_now_iso = self.mod.now_iso

        self.mod.gh_available = lambda: True
        self.mod.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.mod.gh_current_login = lambda: "danielraffel"
        dispatched = {}
        self.mod.gh_workflow_dispatch = (
            lambda repository, workflow_file, ref, fields: dispatched.update(
                {
                    "repository": repository,
                    "workflow_file": workflow_file,
                    "ref": ref,
                    "fields": dict(fields),
                }
            )
        )
        self.mod.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: None
        self.mod.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="build",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json=None,
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json="\"namespace-profile-big-apple\"",
                        wait=False,
                    )
                )
        finally:
            self.mod.gh_available = original_gh_available
            self.mod.resolve_github_repository = original_resolve_repo
            self.mod.gh_current_login = original_current_login
            self.mod.gh_workflow_dispatch = original_dispatch
            self.mod.gh_find_dispatched_run = original_find
            self.mod.now_iso = original_now_iso

        self.assertEqual(exit_code, 0)
        self.assertEqual(
            dispatched["fields"]["macos_runner_selector_json"],
            "\"namespace-profile-big-apple\"",
        )

    def test_cmd_cloud_run_rejects_build_leg_override_for_docs_check(self):
        original_gh_available = self.mod.gh_available
        self.mod.gh_available = lambda: True
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="docs-check",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json=None,
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json="\"namespace-profile-big-apple\"",
                        wait=False,
                    )
                )
        finally:
            self.mod.gh_available = original_gh_available

        output = buf.getvalue()
        self.assertEqual(exit_code, 1)
        self.assertIn("--macos-runner-selector-json is not supported", output)

    def test_cmd_cloud_run_dispatches_waits_and_persists_record(self):
        original_gh_available = self.mod.gh_available
        original_resolve_repo = self.mod.resolve_github_repository
        original_current_login = self.mod.gh_current_login
        original_dispatch = self.mod.gh_workflow_dispatch
        original_find = self.mod.gh_find_dispatched_run
        original_view = self.mod.gh_run_view
        original_sleep = self.mod.time.sleep
        original_now_iso = self.mod.now_iso

        self.mod.gh_available = lambda: True
        self.mod.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.mod.gh_current_login = lambda: "danielraffel"
        dispatched = {}
        self.mod.gh_workflow_dispatch = (
            lambda repository, workflow_file, ref, fields: dispatched.update(
                {
                    "repository": repository,
                    "workflow_file": workflow_file,
                    "ref": ref,
                    "fields": dict(fields),
                }
            )
        )
        self.mod.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: {
            "databaseId": 98765,
            "headBranch": ref,
            "headSha": "e" * 40,
            "status": "in_progress",
            "conclusion": "",
            "url": "https://example.test/runs/98765",
            "createdAt": "2026-04-04T12:00:05+00:00",
            "updatedAt": "2026-04-04T12:00:05+00:00",
            "workflowName": "Docs Consistency",
            "match_ambiguous": False,
        }
        self.mod.gh_run_view = lambda repository, run_id: {
            "databaseId": run_id,
            "status": "completed",
            "conclusion": "success",
            "url": "https://example.test/runs/98765",
            "headSha": "e" * 40,
            "headBranch": "feature/cloud",
            "workflowName": "Docs Consistency",
            "createdAt": "2026-04-04T12:00:05+00:00",
            "updatedAt": "2026-04-04T12:00:10+00:00",
            "jobs": [
                {
                    "name": "Validate docs consistency",
                    "status": "completed",
                    "conclusion": "success",
                    "startedAt": "2026-04-04T12:00:06+00:00",
                    "completedAt": "2026-04-04T12:00:10+00:00",
                }
            ],
        }
        self.mod.time.sleep = lambda _: None
        self.mod.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="docs-check",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json=None,
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json=None,
                        wait=True,
                    )
                )
        finally:
            self.mod.gh_available = original_gh_available
            self.mod.resolve_github_repository = original_resolve_repo
            self.mod.gh_current_login = original_current_login
            self.mod.gh_workflow_dispatch = original_dispatch
            self.mod.gh_find_dispatched_run = original_find
            self.mod.gh_run_view = original_view
            self.mod.time.sleep = original_sleep
            self.mod.now_iso = original_now_iso

        self.assertEqual(exit_code, 0)
        self.assertEqual(dispatched["workflow_file"], "docs-check.yml")
        self.assertEqual(
            dispatched["fields"],
            {
                "runner_provider": "namespace",
                "runner_selector_json": "\"namespace-profile-default\"",
            },
        )
        records = self.mod.list_cloud_records()
        self.assertEqual(len(records), 1)
        self.assertEqual(records[0]["run_id"], 98765)
        self.assertEqual(records[0]["provider_resolved"], "namespace")
        self.assertEqual(records[0]["runner_selector_json"], "\"namespace-profile-default\"")
        self.assertEqual(records[0]["conclusion"], "success")

    def test_cmd_cloud_run_explicit_runner_selector_overrides_config_default(self):
        original_gh_available = self.mod.gh_available
        original_resolve_repo = self.mod.resolve_github_repository
        original_current_login = self.mod.gh_current_login
        original_dispatch = self.mod.gh_workflow_dispatch
        original_find = self.mod.gh_find_dispatched_run
        original_now_iso = self.mod.now_iso

        self.mod.gh_available = lambda: True
        self.mod.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.mod.gh_current_login = lambda: "danielraffel"
        dispatched = {}
        self.mod.gh_workflow_dispatch = (
            lambda repository, workflow_file, ref, fields: dispatched.update(
                {
                    "repository": repository,
                    "workflow_file": workflow_file,
                    "ref": ref,
                    "fields": dict(fields),
                }
            )
        )
        self.mod.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: None
        self.mod.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="docs-check",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json="\"namespace-profile-big-apple\"",
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json=None,
                        wait=False,
                    )
                )
        finally:
            self.mod.gh_available = original_gh_available
            self.mod.resolve_github_repository = original_resolve_repo
            self.mod.gh_current_login = original_current_login
            self.mod.gh_workflow_dispatch = original_dispatch
            self.mod.gh_find_dispatched_run = original_find
            self.mod.now_iso = original_now_iso

        self.assertEqual(exit_code, 0)
        self.assertEqual(
            dispatched["fields"],
            {
                "runner_provider": "namespace",
                "runner_selector_json": "\"namespace-profile-big-apple\"",
            },
        )
        records = self.mod.list_cloud_records()
        self.assertEqual(records[0]["runner_selector_json"], "\"namespace-profile-big-apple\"")

    def test_cmd_cloud_status_shows_runner_selector(self):
        self.mod.save_cloud_record(
            {
                "dispatch_id": "sel123def456",
                "workflow_key": "docs-check",
                "workflow_name": "Docs Consistency",
                "workflow_file": "docs-check.yml",
                "repository": "danielraffel/pulp",
                "requested_ref": "feature/cloud",
                "provider_requested": "namespace",
                "runner_selector_json": "\"namespace-profile-default\"",
                "status": "completed",
                "conclusion": "success",
                "run_id": 98765,
                "started_at": "2026-04-04T12:00:06+00:00",
                "completed_at": "2026-04-04T12:00:30+00:00",
                "queue_delay_secs": 1,
                "duration_secs": 24,
                "usage_summary": {
                    "instances_count": 2,
                    "provider_runtime_secs": 75,
                    "machine_shapes": [
                        {
                            "os": "linux",
                            "arch": "amd64",
                            "virtual_cpu": 4,
                            "memory_megabytes": 8192,
                            "profile_tag": "namespace-profile-default",
                            "count": 2,
                            "duration_secs": 75,
                        }
                    ],
                },
                "cost_summary": {
                    "status": "unavailable",
                    "reason": "Namespace CLI does not expose billing totals; provider runtime is shown instead.",
                },
                "jobs": [
                    {
                        "name": "Validate docs consistency",
                        "status": "completed",
                        "conclusion": "success",
                        "started_at": "2026-04-04T12:00:09+00:00",
                        "completed_at": "2026-04-04T12:00:30+00:00",
                    }
                ],
                "dispatched_at": "2026-04-04T12:00:00+00:00",
                "updated_at": "2026-04-04T12:01:00+00:00",
            }
        )

        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_cloud_status(
                SimpleNamespace(identifier="latest", refresh=False, limit=5)
            )

        self.assertEqual(exit_code, 0)
        self.assertIn("runner selector: namespace-profile-default", buf.getvalue())
        self.assertIn("queue delay: 1s", buf.getvalue())
        self.assertIn("elapsed: 24s", buf.getvalue())
        self.assertIn("provider usage: 2 Namespace instance(s) runtime=1m15s", buf.getvalue())
        self.assertIn("namespace-profile-default: linux/amd64 4 vCPU 8 GB x2 runtime=1m15s", buf.getvalue())
        self.assertIn("cost: unavailable", buf.getvalue())
        self.assertIn("duration=21s", buf.getvalue())

    def test_cmd_cloud_defaults_reports_effective_providers_and_sources(self):
        config = json.loads(self.config_path.read_text())
        config["github_actions"]["defaults"]["provider"] = "namespace"
        del config["github_actions"]["workflows"]["docs-check"]["providers"]["namespace"]["runner_selector_json"]
        self.config_path.write_text(json.dumps(config) + "\n")

        original_gh_available = self.mod.gh_available
        original_repo_variables = self.mod.gh_repo_variables
        self.mod.gh_available = lambda: True
        self.mod.gh_repo_variables = lambda repository: {
            "PULP_NAMESPACE_DOCS_CHECK_RUNS_ON_JSON": "\"namespace-profile-docs\"",
            "PULP_NAMESPACE_BUILD_LINUX_RUNS_ON_JSON": "\"namespace-profile-linux\"",
            "PULP_NAMESPACE_BUILD_WINDOWS_RUNS_ON_JSON": "\"namespace-profile-windows\"",
        }
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_defaults(SimpleNamespace())
        finally:
            self.mod.gh_available = original_gh_available
            self.mod.gh_repo_variables = original_repo_variables

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("configured default provider: namespace", output)
        self.assertIn("billing estimates: USD period-day=1 (estimated; verify provider pricing)", output)
        self.assertIn("provider billing truth: disabled (opt-in; off by default)", output)
        self.assertIn("build: Build and Test (build.yml)", output)
        self.assertIn("linux_runner_selector_json: namespace-profile-default", output)
        self.assertIn("docs-check: Docs Consistency (docs-check.yml)", output)
        self.assertIn("runner_selector_json: namespace-profile-docs (repo variable PULP_NAMESPACE_DOCS_CHECK_RUNS_ON_JSON)", output)
        self.assertIn("validate: Plugin Validation (validate.yml)", output)
        self.assertIn("default provider: github-hosted (workflow fallback", output)

    def test_cmd_cloud_defaults_handles_invalid_timing_config(self):
        config = json.loads(self.config_path.read_text())
        config["github_actions"]["defaults"]["wait_poll_secs"] = "not-an-int"
        self.config_path.write_text(json.dumps(config) + "\n")

        original_gh_available = self.mod.gh_available
        self.mod.gh_available = lambda: False
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_defaults(SimpleNamespace())
        finally:
            self.mod.gh_available = original_gh_available

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("repository: danielraffel/pulp", output)
        self.assertIn("note: github_actions.defaults.wait_poll_secs must be an integer.", output)
        self.assertIn("configured default workflow: build", output)
        self.assertIn("configured default provider: github-hosted", output)

    def test_estimate_cloud_record_cost_uses_namespace_profile_rate(self):
        config = json.loads(self.config_path.read_text())
        config["telemetry"] = {
            "billing": {
                "currency": "USD",
                "namespace_profile_tag_rates_per_hour": {
                    "namespace-profile-default": 0.5
                }
            }
        }

        summary = self.mod.estimate_cloud_record_cost(
            {
                "provider_requested": "namespace",
                "provider_resolved": "namespace",
                "provider_metadata": {
                    "namespace_instances": [
                        {
                            "profile_tag": "namespace-profile-default",
                            "os": "linux",
                            "arch": "amd64",
                            "virtual_cpu": 4,
                            "memory_megabytes": 8192,
                            "duration_secs": 7200,
                        }
                    ]
                },
            },
            config,
        )

        self.assertEqual(summary["status"], "estimated")
        self.assertEqual(summary["currency"], "USD")
        self.assertAlmostEqual(summary["estimated_total"], 1.0)
        self.assertEqual(summary["reason"], "estimated; verify provider pricing")

    def test_fetch_github_repo_actions_billing_summary_sums_repo_usage(self):
        config = {
            "telemetry": {
                "billing": {
                    "enable_provider_reported_totals": True,
                }
            }
        }

        original_gh_available = self.mod.gh_available
        original_gh_api_json = self.mod.gh_api_json
        original_billing_window = self.mod.billing_period_window
        self.mod.gh_available = lambda: True
        self.mod.billing_period_window = lambda start_day, now_dt=None: (
            datetime(2026, 3, 15, tzinfo=timezone.utc),
            datetime(2026, 4, 15, tzinfo=timezone.utc),
        )

        def fake_gh_api_json(path, fields=None):
            if path == "/repos/danielraffel/pulp":
                return ({"owner": {"login": "danielraffel", "type": "User"}}, "")
            if path == "/users/danielraffel/settings/billing/usage":
                if fields == {"year": 2026, "month": 3}:
                    return (
                        {
                            "usageItems": [
                                {
                                    "date": "2026-03-14",
                                    "product": "Actions",
                                    "repositoryName": "danielraffel/pulp",
                                    "netAmount": 1.0,
                                },
                                {
                                    "date": "2026-03-15",
                                    "product": "Actions",
                                    "repositoryName": "danielraffel/pulp",
                                    "netAmount": 2.0,
                                },
                            ]
                        },
                        "",
                    )
                if fields == {"year": 2026, "month": 4}:
                    return (
                        {
                            "usageItems": [
                                {
                                    "date": "2026-04-01",
                                    "product": "Actions",
                                    "repositoryName": "danielraffel/pulp",
                                    "netAmount": 3.5,
                                },
                                {
                                    "date": "2026-04-02",
                                    "product": "Packages",
                                    "repositoryName": "danielraffel/pulp",
                                    "netAmount": 9.0,
                                },
                                {
                                    "date": "2026-04-03",
                                    "product": "Actions",
                                    "repositoryName": "other/repo",
                                    "netAmount": 7.0,
                                },
                            ]
                        },
                        "",
                    )
            return (None, "unexpected call")

        self.mod.gh_api_json = fake_gh_api_json
        try:
            summary = self.mod.fetch_github_repo_actions_billing_summary("danielraffel/pulp", config)
        finally:
            self.mod.gh_available = original_gh_available
            self.mod.gh_api_json = original_gh_api_json
            self.mod.billing_period_window = original_billing_window

        self.assertEqual(summary["status"], "actual")
        self.assertEqual(summary["currency"], "USD")
        self.assertAlmostEqual(summary["actual_total"], 5.5)
        self.assertEqual(summary["matched_items"], 2)
        self.assertEqual(summary["reason"], "actual when available")

    def test_cmd_cloud_history_shows_estimated_cost_and_period_total(self):
        config = json.loads(self.config_path.read_text())
        config["telemetry"] = {
            "billing": {
                "currency": "USD",
                "namespace_profile_tag_rates_per_hour": {
                    "namespace-profile-default": 0.5
                }
            }
        }
        self.config_path.write_text(json.dumps(config) + "\n")

        self.mod.save_cloud_record(
            {
                "dispatch_id": "hist123def456",
                "workflow_key": "docs-check",
                "workflow_name": "Docs Consistency",
                "workflow_file": "docs-check.yml",
                "repository": "danielraffel/pulp",
                "requested_ref": "feature/cloud",
                "provider_requested": "namespace",
                "provider_resolved": "namespace",
                "status": "completed",
                "conclusion": "success",
                "run_id": 98765,
                "duration_secs": 24,
                "completed_at": "2026-04-04T12:00:30+00:00",
                "usage_summary": {
                    "instances_count": 1,
                    "provider_runtime_secs": 3600,
                    "machine_shapes": [
                        {
                            "os": "linux",
                            "arch": "amd64",
                            "virtual_cpu": 4,
                            "memory_megabytes": 8192,
                            "profile_tag": "namespace-profile-default",
                            "count": 1,
                            "duration_secs": 3600,
                        }
                    ],
                },
                "provider_metadata": {
                    "namespace_instances": [
                        {
                            "profile_tag": "namespace-profile-default",
                            "os": "linux",
                            "arch": "amd64",
                            "virtual_cpu": 4,
                            "memory_megabytes": 8192,
                            "duration_secs": 3600,
                        }
                    ]
                },
            }
        )

        original_billing_period_window = self.mod.billing_period_window
        self.mod.billing_period_window = lambda start_day, now_dt=None: (
            datetime(2026, 4, 1, tzinfo=timezone.utc),
            datetime(2026, 5, 1, tzinfo=timezone.utc),
        )
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_history(
                    SimpleNamespace(workflow=None, provider=None, limit=10)
                )
        finally:
            self.mod.billing_period_window = original_billing_period_window

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("cost=est $0.50", output)
        self.assertIn("period cost: est $0.50 over 1 run(s); estimated; verify provider pricing", output)

    def test_cmd_cloud_history_shows_provider_reported_github_billing_when_enabled(self):
        config = json.loads(self.config_path.read_text())
        config["telemetry"] = {
            "billing": {
                "enable_provider_reported_totals": True,
            }
        }
        self.config_path.write_text(json.dumps(config) + "\n")

        self.mod.save_cloud_record(
            {
                "dispatch_id": "histgh123456",
                "workflow_key": "build",
                "workflow_name": "Build and Test",
                "workflow_file": "build.yml",
                "repository": "danielraffel/pulp",
                "requested_ref": "feature/cloud",
                "provider_requested": "github-hosted",
                "provider_resolved": "github-hosted",
                "status": "completed",
                "conclusion": "success",
                "run_id": 12345,
                "duration_secs": 30,
                "completed_at": "2026-04-04T12:00:30+00:00",
            }
        )

        original_fetch = self.mod.fetch_github_repo_actions_billing_summary
        self.mod.fetch_github_repo_actions_billing_summary = lambda repository, cfg: {
            "status": "actual",
            "currency": "USD",
            "actual_total": 2.7,
            "reason": "actual when available",
        }
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_history(
                    SimpleNamespace(workflow=None, provider=None, limit=10)
                )
        finally:
            self.mod.fetch_github_repo_actions_billing_summary = original_fetch

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("github repo billing: actual $2.70 current period (repo-wide)", output)

    def test_cmd_cloud_compare_reports_provider_medians(self):
        config = json.loads(self.config_path.read_text())
        config["telemetry"] = {
            "billing": {
                "currency": "USD",
                "github_hosted_job_os_rates_per_minute": {
                    "linux": 0.01
                },
                "namespace_profile_tag_rates_per_hour": {
                    "namespace-profile-default": 0.5
                }
            }
        }
        self.config_path.write_text(json.dumps(config) + "\n")

        self.mod.save_cloud_record(
            {
                "dispatch_id": "cmpns123456",
                "workflow_key": "build",
                "provider_requested": "namespace",
                "provider_resolved": "namespace",
                "status": "completed",
                "conclusion": "success",
                "completed_at": "2026-04-04T12:00:30+00:00",
                "duration_secs": 120,
                "queue_delay_secs": 5,
                "usage_summary": {
                    "provider_runtime_secs": 3600,
                    "machine_shapes": [
                        {
                            "os": "linux",
                            "arch": "amd64",
                            "virtual_cpu": 4,
                            "memory_megabytes": 8192,
                            "profile_tag": "namespace-profile-default",
                            "count": 1,
                            "duration_secs": 3600,
                        }
                    ],
                },
                "provider_metadata": {
                    "namespace_instances": [
                        {
                            "profile_tag": "namespace-profile-default",
                            "os": "linux",
                            "arch": "amd64",
                            "virtual_cpu": 4,
                            "memory_megabytes": 8192,
                            "duration_secs": 3600,
                        }
                    ]
                },
            }
        )
        self.mod.save_cloud_record(
            {
                "dispatch_id": "cmpgh123456",
                "workflow_key": "build",
                "provider_requested": "github-hosted",
                "provider_resolved": "github-hosted",
                "status": "completed",
                "conclusion": "success",
                "completed_at": "2026-04-04T12:10:30+00:00",
                "duration_secs": 180,
                "queue_delay_secs": 15,
                "jobs": [
                    {
                        "name": "Linux (x64) [github-hosted]",
                        "started_at": "2026-04-04T12:07:30+00:00",
                        "completed_at": "2026-04-04T12:10:30+00:00",
                    }
                ],
            }
        )

        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_cloud_compare(SimpleNamespace(workflow="build"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn(
            "github-hosted: runs=1 success=1/1 median_elapsed=3m00s median_queue=15s median_cost=est $0.03 latest_success=2026-04-04T12:10:30+00:00",
            output,
        )
        self.assertIn(
            "namespace: runs=1 success=1/1 median_elapsed=2m00s median_queue=5s median_provider_time=1h00m00s median_cost=est $0.50 latest_success=2026-04-04T12:00:30+00:00",
            output,
        )
        self.assertIn("note: estimated; verify provider pricing", output)

    def test_cmd_cloud_recommend_prefers_fastest_observed_provider(self):
        self.mod.save_cloud_record(
            {
                "dispatch_id": "recns123456",
                "workflow_key": "build",
                "provider_requested": "namespace",
                "provider_resolved": "namespace",
                "status": "completed",
                "conclusion": "success",
                "completed_at": "2026-04-04T12:00:30+00:00",
                "duration_secs": 120,
            }
        )
        self.mod.save_cloud_record(
            {
                "dispatch_id": "recgh123456",
                "workflow_key": "build",
                "provider_requested": "github-hosted",
                "provider_resolved": "github-hosted",
                "status": "completed",
                "conclusion": "success",
                "completed_at": "2026-04-04T12:10:30+00:00",
                "duration_secs": 180,
            }
        )

        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_cloud_recommend(SimpleNamespace(workflow="build"))

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Recommended provider for build: namespace (fastest observed median)", output)
        self.assertIn("note: estimated; verify provider pricing", output)

    def test_cmd_cloud_run_wait_fails_when_refresh_cannot_fetch_github_state(self):
        original_gh_available = self.mod.gh_available
        original_resolve_repo = self.mod.resolve_github_repository
        original_current_login = self.mod.gh_current_login
        original_dispatch = self.mod.gh_workflow_dispatch
        original_find = self.mod.gh_find_dispatched_run
        original_view = self.mod.gh_run_view
        original_sleep = self.mod.time.sleep
        original_now_iso = self.mod.now_iso

        self.mod.gh_available = lambda: True
        self.mod.resolve_github_repository = lambda settings: "danielraffel/pulp"
        self.mod.gh_current_login = lambda: "danielraffel"
        self.mod.gh_workflow_dispatch = lambda repository, workflow_file, ref, fields: None
        self.mod.gh_find_dispatched_run = lambda repository, workflow_file, ref, dispatched_at, timeout_secs: {
            "databaseId": 98765,
            "workflowName": "Docs Consistency",
            "headBranch": "feature/cloud",
            "headSha": "a" * 40,
            "status": "in_progress",
            "conclusion": "",
            "url": "https://example.test/runs/98765",
            "createdAt": "2026-04-04T12:00:05+00:00",
            "updatedAt": "2026-04-04T12:00:06+00:00",
            "jobs": [],
        }
        self.mod.gh_run_view = lambda repository, run_id: None
        self.mod.time.sleep = lambda _: None
        self.mod.now_iso = lambda: "2026-04-04T12:00:00+00:00"
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_run(
                    SimpleNamespace(
                        workflow="docs-check",
                        branch="feature/cloud",
                        provider="namespace",
                        runner_selector_json=None,
                        linux_runner_selector_json=None,
                        windows_runner_selector_json=None,
                        macos_runner_selector_json=None,
                        wait=True,
                    )
                )
        finally:
            self.mod.gh_available = original_gh_available
            self.mod.resolve_github_repository = original_resolve_repo
            self.mod.gh_current_login = original_current_login
            self.mod.gh_workflow_dispatch = original_dispatch
            self.mod.gh_find_dispatched_run = original_find
            self.mod.gh_run_view = original_view
            self.mod.time.sleep = original_sleep
            self.mod.now_iso = original_now_iso

        output = buf.getvalue()
        self.assertEqual(exit_code, 1)
        self.assertIn("Error: Failed to refresh GitHub run 98765 from danielraffel/pulp.", output)

    def test_cmd_status_includes_recent_cloud_summary(self):
        self.mod.save_cloud_record(
            {
                "dispatch_id": "abc123def456",
                "workflow_key": "docs-check",
                "requested_ref": "feature/cloud",
                "provider_requested": "namespace",
                "status": "completed",
                "conclusion": "success",
                "run_id": 98765,
                "dispatched_at": "2026-04-04T12:00:00+00:00",
                "updated_at": "2026-04-04T12:01:00+00:00",
            }
        )

        original_current_branch = self.mod.current_branch
        original_utm_status = self.mod.utmctl_vm_status
        original_ssh_reachable = self.mod.ssh_reachable
        self.mod.current_branch = lambda: "feature/cloud"
        self.mod.utmctl_vm_status = lambda vm_name: "stopped"
        self.mod.ssh_reachable = lambda host, timeout=5: True
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_status(SimpleNamespace())
        finally:
            self.mod.current_branch = original_current_branch
            self.mod.utmctl_vm_status = original_utm_status
            self.mod.ssh_reachable = original_ssh_reachable

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Cloud defaults: workflow=build provider=github-hosted", output)
        self.assertIn("Cloud (latest 5 known to this machine):", output)
        self.assertIn("docs-check", output)
        self.assertIn("gha#98765", output)

    def test_cmd_cloud_status_refresh_uses_record_repository(self):
        self.mod.save_cloud_record(
            {
                "dispatch_id": "repo123def456",
                "workflow_key": "docs-check",
                "workflow_name": "Docs Consistency",
                "workflow_file": "docs-check.yml",
                "repository": "other-owner/other-repo",
                "requested_ref": "feature/cloud",
                "provider_requested": "github-hosted",
                "status": "in_progress",
                "run_id": 77777,
                "dispatched_at": "2026-04-04T12:00:00+00:00",
                "updated_at": "2026-04-04T12:01:00+00:00",
            }
        )

        original_gh_available = self.mod.gh_available
        original_view = self.mod.gh_run_view
        seen = {}
        self.mod.gh_available = lambda: True
        self.mod.gh_run_view = lambda repository, run_id: (
            seen.update({"repository": repository, "run_id": run_id}) or {
                "databaseId": 77777,
                "workflowName": "Docs Consistency",
                "headBranch": "feature/cloud",
                "headSha": "a" * 40,
                "status": "completed",
                "conclusion": "success",
                "url": "https://example.test/runs/77777",
                "createdAt": "2026-04-04T12:00:05+00:00",
                "updatedAt": "2026-04-04T12:00:30+00:00",
                "jobs": [],
            }
        )
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cloud_status(
                    SimpleNamespace(identifier="latest", refresh=True, limit=5)
                )
        finally:
            self.mod.gh_available = original_gh_available
            self.mod.gh_run_view = original_view

        self.assertEqual(exit_code, 0)
        self.assertEqual(seen["repository"], "other-owner/other-repo")
        self.assertEqual(seen["run_id"], 77777)

    def test_cmd_status_handles_invalid_cloud_defaults_config(self):
        config = json.loads(self.config_path.read_text())
        config["github_actions"]["defaults"]["wait_poll_secs"] = "broken"
        self.config_path.write_text(json.dumps(config) + "\n")

        original_current_branch = self.mod.current_branch
        original_utm_status = self.mod.utmctl_vm_status
        original_ssh_reachable = self.mod.ssh_reachable
        self.mod.current_branch = lambda: "feature/cloud"
        self.mod.utmctl_vm_status = lambda vm_name: "stopped"
        self.mod.ssh_reachable = lambda host, timeout=5: True
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_status(SimpleNamespace())
        finally:
            self.mod.current_branch = original_current_branch
            self.mod.utmctl_vm_status = original_utm_status
            self.mod.ssh_reachable = original_ssh_reachable

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Cloud defaults: workflow=build provider=github-hosted", output)
        self.assertIn("note: github_actions.defaults.wait_poll_secs must be an integer.", output)

    def test_cmd_status_period_cost_uses_full_cloud_history(self):
        config = json.loads(self.config_path.read_text())
        config["telemetry"] = {
            "billing": {
                "currency": "USD",
                "namespace_profile_tag_rates_per_hour": {
                    "namespace-profile-default": 0.5
                }
            }
        }
        self.config_path.write_text(json.dumps(config) + "\n")

        for index in range(6):
            self.mod.save_cloud_record(
                {
                    "dispatch_id": f"hist{index:02d}abcdef",
                    "workflow_key": "build",
                    "provider_requested": "namespace",
                    "provider_resolved": "namespace",
                    "status": "completed",
                    "conclusion": "success",
                    "completed_at": f"2026-04-04T12:0{index}:30+00:00",
                    "duration_secs": 24,
                    "provider_metadata": {
                        "namespace_instances": [
                            {
                                "profile_tag": "namespace-profile-default",
                                "os": "linux",
                                "arch": "amd64",
                                "virtual_cpu": 4,
                                "memory_megabytes": 8192,
                                "duration_secs": 3600,
                            }
                        ]
                    },
                }
            )

        original_current_branch = self.mod.current_branch
        original_utm_status = self.mod.utmctl_vm_status
        original_ssh_reachable = self.mod.ssh_reachable
        original_billing_period_window = self.mod.billing_period_window
        self.mod.current_branch = lambda: "feature/cloud"
        self.mod.utmctl_vm_status = lambda vm_name: "stopped"
        self.mod.ssh_reachable = lambda host, timeout=5: True
        self.mod.billing_period_window = lambda start_day, now_dt=None: (
            datetime(2026, 4, 1, tzinfo=timezone.utc),
            datetime(2026, 5, 1, tzinfo=timezone.utc),
        )
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_status(SimpleNamespace())
        finally:
            self.mod.current_branch = original_current_branch
            self.mod.utmctl_vm_status = original_utm_status
            self.mod.ssh_reachable = original_ssh_reachable
            self.mod.billing_period_window = original_billing_period_window

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("period cost: est $3.00 over 6 run(s); estimated; verify provider pricing", output)
        self.assertIn("Cloud (latest 5 known to this machine):", output)

    def test_collect_local_ci_cleanup_plan_selects_stale_artifacts(self):
        queue = [
            {
                "id": "pending123456",
                "branch": "feature/pending",
                "sha": "a" * 40,
                "priority": "normal",
                "targets": ["mac"],
                "queued_at": "2026-04-04T12:00:00+00:00",
                "status": "pending",
                "fingerprint": "pending",
                "mode": "run",
                "validation": "full",
            },
            {
                "id": "keep12345678",
                "branch": "feature/keep",
                "sha": "b" * 40,
                "priority": "normal",
                "targets": ["mac"],
                "queued_at": "2026-04-04T12:01:00+00:00",
                "completed_at": "2026-04-04T12:02:00+00:00",
                "status": "completed",
                "fingerprint": "keep",
                "mode": "run",
                "validation": "full",
            },
        ]
        with self.mod.file_lock(self.mod.queue_lock_path(), blocking=True):
            self.mod.save_queue_unlocked(queue)

        (self.state_dir / "bundles").mkdir(parents=True, exist_ok=True)
        (self.state_dir / "bundles" / "pending123456.bundle").write_bytes(b"live")
        (self.state_dir / "bundles" / "stale1234567.bundle").write_bytes(b"stale")

        keep_log_dir = self.state_dir / "logs" / "keep12345678"
        keep_log_dir.mkdir(parents=True, exist_ok=True)
        (keep_log_dir / "mac.log").write_text("keep")
        stale_log_dir = self.state_dir / "logs" / "stale1234567"
        stale_log_dir.mkdir(parents=True, exist_ok=True)
        (stale_log_dir / "mac.log").write_text("stale")

        (self.state_dir / "results").mkdir(parents=True, exist_ok=True)
        (self.state_dir / "results" / "20260404-120000-keep12345678-feature-keep.json").write_text("{}\n")
        (self.state_dir / "results" / "20260404-120100-stale1234567-feature-stale.json").write_text("{}\n")

        prepared_full = self.state_dir / "prepared" / "mac" / "full"
        prepared_full.mkdir(parents=True, exist_ok=True)
        (prepared_full / "marker").write_text("prepared")

        plan = self.mod.collect_local_ci_cleanup_plan(
            queue,
            keep_results=0,
            keep_logs=0,
            keep_bundles=0,
            include_prepared=True,
        )

        bundle_paths = {Path(entry["path"]).name for entry in plan["categories"]["bundles"]}
        log_paths = {Path(entry["path"]).name for entry in plan["categories"]["logs"]}
        result_paths = {Path(entry["path"]).name for entry in plan["categories"]["results"]}
        prepared_paths = {str(Path(entry["path"]).relative_to(self.state_dir)) for entry in plan["categories"]["prepared"]}

        self.assertEqual(bundle_paths, {"stale1234567.bundle"})
        self.assertEqual(log_paths, {"stale1234567"})
        self.assertEqual(result_paths, {"20260404-120100-stale1234567-feature-stale.json"})
        self.assertEqual(prepared_paths, {"prepared/mac/full"})

    def test_cmd_cleanup_dry_run_preserves_files_and_reports_prepared_consequence(self):
        queue = []
        with self.mod.file_lock(self.mod.queue_lock_path(), blocking=True):
            self.mod.save_queue_unlocked(queue)

        (self.state_dir / "bundles").mkdir(parents=True, exist_ok=True)
        stale_bundle = self.state_dir / "bundles" / "stale1234567.bundle"
        stale_bundle.write_bytes(b"stale")
        prepared_full = self.state_dir / "prepared" / "mac" / "full"
        prepared_full.mkdir(parents=True, exist_ok=True)
        (prepared_full / "marker").write_text("prepared")

        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_cleanup(
                SimpleNamespace(
                    apply=False,
                    include_prepared=True,
                    keep_results=0,
                    keep_logs=0,
                    keep_bundles=0,
                )
            )

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertTrue(stale_bundle.exists())
        self.assertTrue(prepared_full.exists())
        self.assertIn("dry run only; re-run with --apply", output)
        self.assertIn("prepared cleanup removes cached build/install state", output)
        self.assertIn("Local CI footprint:", output)

    def test_cmd_cleanup_apply_removes_stale_artifacts_and_preserves_retained_state(self):
        queue = [
            {
                "id": "pending123456",
                "branch": "feature/pending",
                "sha": "a" * 40,
                "priority": "normal",
                "targets": ["mac"],
                "queued_at": "2026-04-04T12:00:00+00:00",
                "status": "pending",
                "fingerprint": "pending",
                "mode": "run",
                "validation": "full",
            },
            {
                "id": "keep12345678",
                "branch": "feature/keep",
                "sha": "b" * 40,
                "priority": "normal",
                "targets": ["mac"],
                "queued_at": "2026-04-04T12:01:00+00:00",
                "completed_at": "2026-04-04T12:02:00+00:00",
                "status": "completed",
                "fingerprint": "keep",
                "mode": "run",
                "validation": "full",
            },
        ]
        with self.mod.file_lock(self.mod.queue_lock_path(), blocking=True):
            self.mod.save_queue_unlocked(queue)

        (self.state_dir / "bundles").mkdir(parents=True, exist_ok=True)
        live_bundle = self.state_dir / "bundles" / "pending123456.bundle"
        live_bundle.write_bytes(b"live")
        stale_bundle = self.state_dir / "bundles" / "stale1234567.bundle"
        stale_bundle.write_bytes(b"stale")

        keep_log_dir = self.state_dir / "logs" / "keep12345678"
        keep_log_dir.mkdir(parents=True, exist_ok=True)
        (keep_log_dir / "mac.log").write_text("keep")
        stale_log_dir = self.state_dir / "logs" / "stale1234567"
        stale_log_dir.mkdir(parents=True, exist_ok=True)
        (stale_log_dir / "mac.log").write_text("stale")

        (self.state_dir / "results").mkdir(parents=True, exist_ok=True)
        keep_result = self.state_dir / "results" / "20260404-120000-keep12345678-feature-keep.json"
        keep_result.write_text("{}\n")
        stale_result = self.state_dir / "results" / "20260404-120100-stale1234567-feature-stale.json"
        stale_result.write_text("{}\n")

        prepared_full = self.state_dir / "prepared" / "mac" / "full"
        prepared_full.mkdir(parents=True, exist_ok=True)
        (prepared_full / "marker").write_text("prepared")

        buf = io.StringIO()
        with redirect_stdout(buf):
            exit_code = self.mod.cmd_cleanup(
                SimpleNamespace(
                    apply=True,
                    include_prepared=True,
                    keep_results=0,
                    keep_logs=0,
                    keep_bundles=0,
                )
            )

        self.assertEqual(exit_code, 0)
        self.assertTrue(live_bundle.exists())
        self.assertTrue(keep_log_dir.exists())
        self.assertTrue(keep_result.exists())
        self.assertFalse(stale_bundle.exists())
        self.assertFalse(stale_log_dir.exists())
        self.assertFalse(stale_result.exists())
        self.assertFalse(prepared_full.exists())

    def test_cmd_cleanup_apply_refuses_to_run_while_job_is_running(self):
        running_queue = [
            {
                "id": "running12345",
                "branch": "feature/running",
                "sha": "c" * 40,
                "priority": "normal",
                "targets": ["mac"],
                "queued_at": "2026-04-04T12:00:00+00:00",
                "status": "running",
                "fingerprint": "running",
                "mode": "run",
                "validation": "full",
            }
        ]
        stale_bundle = self.state_dir / "bundles" / "stale1234567.bundle"
        stale_bundle.parent.mkdir(parents=True, exist_ok=True)
        stale_bundle.write_bytes(b"stale")

        original_load_queue = self.mod.load_queue
        self.mod.load_queue = lambda: list(running_queue)
        buf = io.StringIO()
        try:
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_cleanup(
                    SimpleNamespace(
                        apply=True,
                        dry_run=False,
                        include_prepared=False,
                        keep_results=0,
                        keep_logs=0,
                        keep_bundles=0,
                    )
                )
        finally:
            self.mod.load_queue = original_load_queue

        output = buf.getvalue()
        self.assertEqual(exit_code, 1)
        self.assertTrue(stale_bundle.exists())
        self.assertIn("blocked while local CI jobs are running", output)

    def test_finalize_job_prunes_completed_job_bundle_but_keeps_retained_logs_and_results(self):
        running_job = {
            "id": "job123456789",
            "branch": "feature/job",
            "sha": "a" * 40,
            "priority": "normal",
            "targets": ["mac"],
            "queued_at": "2026-04-04T12:00:00+00:00",
            "status": "running",
            "fingerprint": "job",
            "mode": "run",
            "validation": "full",
        }
        with self.mod.file_lock(self.mod.queue_lock_path(), blocking=True):
            self.mod.save_queue_unlocked([running_job])

        bundle_path = self.state_dir / "bundles" / "job123456789.bundle"
        bundle_path.parent.mkdir(parents=True, exist_ok=True)
        bundle_path.write_bytes(b"bundle")

        log_dir = self.state_dir / "logs" / "job123456789"
        log_dir.mkdir(parents=True, exist_ok=True)
        (log_dir / "mac.log").write_text("keep")

        result_path = self.state_dir / "results" / "20260404-120000-job123456789-feature-job.json"
        result_path.parent.mkdir(parents=True, exist_ok=True)
        result_path.write_text("{}\n")

        self.mod.finalize_job(
            "job123456789",
            {"overall": "pass"},
            result_path,
        )

        self.assertFalse(bundle_path.exists())
        self.assertTrue(log_dir.exists())
        self.assertTrue(result_path.exists())

    def test_cmd_status_reports_local_ci_footprint(self):
        bundle_path = self.state_dir / "bundles" / "job123456789.bundle"
        bundle_path.parent.mkdir(parents=True, exist_ok=True)
        bundle_path.write_bytes(b"bundle")
        prepared_full = self.state_dir / "prepared" / "mac" / "full"
        prepared_full.mkdir(parents=True, exist_ok=True)
        (prepared_full / "marker").write_text("prepared")

        original_current_branch = self.mod.current_branch
        original_utm_status = self.mod.utmctl_vm_status
        original_ssh_reachable = self.mod.ssh_reachable
        self.mod.current_branch = lambda: "feature/cloud"
        self.mod.utmctl_vm_status = lambda vm_name: "stopped"
        self.mod.ssh_reachable = lambda host, timeout=5: True
        try:
            buf = io.StringIO()
            with redirect_stdout(buf):
                exit_code = self.mod.cmd_status(SimpleNamespace())
        finally:
            self.mod.current_branch = original_current_branch
            self.mod.utmctl_vm_status = original_utm_status
            self.mod.ssh_reachable = original_ssh_reachable

        output = buf.getvalue()
        self.assertEqual(exit_code, 0)
        self.assertIn("Local CI footprint: total=", output)
        self.assertIn("bundles:", output)
        self.assertIn("prepared:", output)


if __name__ == "__main__":
    unittest.main()
