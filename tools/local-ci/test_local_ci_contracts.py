#!/usr/bin/env python3
"""Behavior contracts for the local_ci.py extraction roadmap.

These tests are intentionally no-network and no-remote. They pin the seams
called out in MODULE_MAP.md so future extraction PRs can move code without
changing queue, evidence, preflight, source-prep, cleanup, or artifact layout
semantics.
"""

from __future__ import annotations

import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("local_ci.py", module_name="pulp_local_ci_contracts", add_module_dir=True)


class LocalCiContractTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)
        self.state_dir = self.root / "state"
        self.config_path = self.root / "config.json"
        self.config_path.write_text(
            json.dumps(
                {
                    "desktop_automation": {
                        "artifact_root": str(self.root / "desktop-artifacts"),
                        "publish_branch": "desktop-artifacts",
                    },
                    "targets": {
                        "mac": {"type": "local", "enabled": True},
                        "ubuntu": {
                            "type": "ssh",
                            "enabled": True,
                            "host": "ubuntu-primary",
                            "repo_path": "/tmp/pulp",
                        },
                    },
                    "github_actions": {
                        "defaults": {
                            "provider": "github-hosted",
                        },
                    },
                    "defaults": {
                        "targets": ["mac"],
                        "ssh_timeout_secs": 1,
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

    def tearDown(self) -> None:
        if self.prev_home is None:
            os.environ.pop("PULP_LOCAL_CI_HOME", None)
        else:
            os.environ["PULP_LOCAL_CI_HOME"] = self.prev_home
        if self.prev_config is None:
            os.environ.pop("PULP_LOCAL_CI_CONFIG", None)
        else:
            os.environ["PULP_LOCAL_CI_CONFIG"] = self.prev_config
        self.tmpdir.cleanup()

    def test_queue_and_evidence_contracts_are_exact_sha_scoped(self) -> None:
        first, first_created = self.mod.enqueue_job(
            "feature/local-ci",
            "a" * 40,
            "low",
            ["mac"],
            "run",
            "full",
        )
        duplicate, duplicate_created = self.mod.enqueue_job(
            "feature/local-ci",
            "a" * 40,
            "high",
            ["mac"],
            "run",
            "full",
        )
        newer, newer_created = self.mod.enqueue_job(
            "feature/local-ci",
            "b" * 40,
            "normal",
            ["mac"],
            "run",
            "full",
        )

        self.assertTrue(first_created)
        self.assertFalse(duplicate_created)
        self.assertTrue(newer_created)
        self.assertEqual(first["id"], duplicate["id"])
        self.assertEqual(self.mod.load_job(first["id"])["priority"], "high")
        self.assertEqual(self.mod.load_job(first["id"])["overall"], "superseded")
        self.assertEqual(self.mod.load_job(first["id"])["superseded_by"], newer["id"])

        older_result = {
            "id": "older",
            "branch": "feature/local-ci",
            "sha": newer["sha"],
            "validation": "full",
            "completed_at": "2026-06-08T01:00:00Z",
            "results": [{"target": "mac", "status": "pass", "duration_secs": 10}],
        }
        failing_result = {
            **older_result,
            "id": "failing",
            "completed_at": "2026-06-08T02:00:00Z",
            "results": [{"target": "mac", "status": "fail", "duration_secs": 1}],
        }
        newer_result = {
            **older_result,
            "id": "newer",
            "completed_at": "2026-06-08T03:00:00Z",
            "results": [{"target": "mac", "status": "pass", "duration_secs": 7}],
        }

        self.mod.update_evidence_index(older_result, self.mod.results_dir() / "older.json")
        self.mod.update_evidence_index(failing_result, self.mod.results_dir() / "failing.json")
        self.mod.update_evidence_index(newer_result, self.mod.results_dir() / "newer.json")

        groups = self.mod.collect_evidence_groups(branch="feature/local-ci", sha=newer["sha"])
        record = groups["full"][0]["targets"]["mac"]
        self.assertEqual(record["completed_at"], "2026-06-08T03:00:00Z")
        self.assertEqual(record["status"], "pass")
        self.assertEqual(record["duration_secs"], 7)

    def test_target_preflight_contract_handles_unreachable_and_namespace_failover(self) -> None:
        config = json.loads(self.config_path.read_text())
        with mock.patch.object(self.mod, "ssh_reachable", return_value=False):
            with self.assertRaisesRegex(ValueError, "Pass --allow-unreachable-targets"):
                self.mod.build_submission_metadata(
                    config,
                    "feature/local-ci",
                    "c" * 40,
                    ["ubuntu"],
                    "normal",
                    "full",
                    allow_root_mismatch=False,
                    allow_unreachable_targets=False,
                )

            allowed = self.mod.build_submission_metadata(
                config,
                "feature/local-ci",
                "c" * 40,
                ["ubuntu"],
                "normal",
                "full",
                allow_root_mismatch=False,
                allow_unreachable_targets=True,
            )

        self.assertEqual(allowed["target_hosts"]["ubuntu"]["status"], "unreachable")
        self.assertIn("ubuntu: ssh host ubuntu-primary is down", allowed["target_hosts"]["ubuntu"]["error"])

        config["github_actions"]["defaults"]["provider"] = "namespace"
        with mock.patch.object(self.mod, "ssh_reachable", return_value=False):
            failed_over = self.mod.build_submission_metadata(
                config,
                "feature/local-ci",
                "c" * 40,
                ["ubuntu"],
                "normal",
                "full",
                allow_root_mismatch=False,
                allow_unreachable_targets=False,
            )

        self.assertEqual(failed_over["target_hosts"]["ubuntu"]["status"], "namespace-failover")
        self.assertEqual(failed_over["namespace_failover_targets"], ["ubuntu"])
        self.assertNotIn("error", failed_over["target_hosts"]["ubuntu"])

    def test_source_preparation_contract_scopes_cache_and_rewrites_launch_commands(self) -> None:
        request = {
            "mode": "exact-sha",
            "sha": "d" * 40,
            "prepare_command": "cmake --build build",
            "prepare_timeout_secs": 30.0,
        }
        same_request = dict(request)
        different_prepare = {**request, "prepare_command": "cmake --build build --target pulp"}

        self.assertEqual(
            self.mod.desktop_source_cache_key(request),
            self.mod.desktop_source_cache_key(same_request),
        )
        self.assertNotEqual(
            self.mod.desktop_source_cache_key(request),
            self.mod.desktop_source_cache_key(different_prepare),
        )

        source_root = self.root / "prepared-source"
        rewritten = self.mod.rewrite_launch_command_for_source_root("./build/pulp --version", source_root)
        self.assertEqual(rewritten, f"{source_root}/build/pulp --version")
        self.assertEqual(
            self.mod.prepared_state_root("mac", " SMOKE "),
            self.state_dir / "prepared" / "mac" / "smoke",
        )
        self.assertTrue(self.mod.should_reuse_prepared_state({"targets": ["mac"]}))
        self.assertFalse(self.mod.should_reuse_prepared_state({"targets": ["mac", "ubuntu"]}))

    def test_cleanup_contract_retains_live_artifacts_and_prunes_only_requested_classes(self) -> None:
        self.mod.ensure_state_dirs()
        live_job = {"id": "live", "status": "running"}
        retained_job = {"id": "retained", "status": "completed"}

        for job_id in ("live", "retained", "old"):
            (self.mod.bundles_dir() / f"{job_id}.bundle").write_text(job_id)
            (self.mod.logs_dir() / job_id).mkdir(parents=True)
            (self.mod.logs_dir() / job_id / "mac.log").write_text(job_id)
            (self.mod.results_dir() / f"result-full-{job_id}.json").write_text("{}")

        prepared = self.mod.prepared_dir() / "mac" / "full"
        prepared.mkdir(parents=True)
        (prepared / "stamp").write_text("ok")

        without_prepared = self.mod.collect_local_ci_cleanup_plan(
            [live_job, retained_job],
            keep_results=0,
            keep_logs=0,
            keep_bundles=0,
            include_prepared=False,
        )
        with_prepared = self.mod.collect_local_ci_cleanup_plan(
            [live_job, retained_job],
            keep_results=0,
            keep_logs=0,
            keep_bundles=0,
            include_prepared=True,
        )

        bundle_paths = {entry["path"].name for entry in without_prepared["categories"]["bundles"]}
        log_paths = {entry["path"].name for entry in without_prepared["categories"]["logs"]}
        result_paths = {entry["path"].name for entry in without_prepared["categories"]["results"]}

        self.assertEqual(bundle_paths, {"old.bundle", "retained.bundle"})
        self.assertEqual(log_paths, {"old"})
        self.assertEqual(result_paths, {"result-full-old.json"})
        self.assertEqual(without_prepared["categories"]["prepared"], [])
        self.assertEqual([entry["path"] for entry in with_prepared["categories"]["prepared"]], [prepared])

    def test_artifact_publish_contract_uses_stable_bundle_layout_and_urls(self) -> None:
        config = json.loads(self.config_path.read_text())
        run_bundle = self.mod.create_desktop_run_bundle(config, "mac", "inspect")
        publish_bundle = self.mod.create_desktop_publish_bundle(config)

        self.assertEqual(run_bundle.parent.name, "inspect")
        self.assertEqual(run_bundle.parent.parent.name, "mac")
        self.assertTrue((run_bundle / "screenshots").is_dir())
        self.assertEqual(publish_bundle.parent.name, "_published")
        self.assertTrue((publish_bundle / "assets").is_dir())

        report_dir = self.root / "report-20260608"
        report_dir.mkdir()
        (report_dir / "index.json").write_text(json.dumps({"ok": True}) + "\n")
        report = {
            "output_dir": str(report_dir),
            "runs": [
                {
                    "label": "mac-smoke",
                    "target": "mac",
                    "action": "inspect",
                    "artifacts": {"snapshot": "runs/mac/snapshot.png", "count": 1},
                }
            ],
        }
        git_commands: list[list[str]] = []

        def fake_run_git(args, *, cwd, check=True):
            git_commands.append(list(args))
            if args[:2] == ["worktree", "add"]:
                Path(args[-2]).mkdir(parents=True, exist_ok=True)
            if args[:2] == ["status", "--short"]:
                return mock.Mock(stdout=" M desktop-automation\n")
            if args[:2] == ["ls-remote", "--heads"]:
                return mock.Mock(stdout="")
            return mock.Mock(stdout="")

        with mock.patch.object(self.mod, "_run_git", side_effect=fake_run_git), \
             mock.patch.object(self.mod, "_reset_local_worktree"), \
             mock.patch.object(self.mod, "git_origin_http_url", return_value="https://github.com/danielraffel/pulp"):
            published = self.mod.publish_report_to_branch(config, report)

        self.assertEqual(published["mode"], "branch")
        self.assertEqual(published["branch"], "desktop-artifacts")
        self.assertEqual(published["report_path"], "desktop-automation/reports/report-20260608")
        self.assertEqual(published["latest_path"], "desktop-automation/latest")
        self.assertEqual(
            published["runs"][0]["artifact_urls"]["snapshot"],
            "https://github.com/danielraffel/pulp/blob/desktop-artifacts/desktop-automation/latest/runs/mac/snapshot.png",
        )
        self.assertIn(["add", "desktop-automation"], git_commands)
        self.assertIn(["push", "origin", "HEAD:desktop-artifacts"], git_commands)


if __name__ == "__main__":
    unittest.main()
