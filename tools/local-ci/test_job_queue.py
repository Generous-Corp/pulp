#!/usr/bin/env python3
"""Tests for the job_queue normalize/load/save helpers.

Split out of test_local_ci.py (roadmap P11-3) so the test surface mirrors
the extracted job_queue module. The harness still loads the local_ci.py
orchestrator, which re-exports the job_queue symbols.
"""

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



class JobQueueTests(unittest.TestCase):
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
        self.assertEqual(normalized["branch"], "feature/local-ci")
        self.assertEqual(normalized["sha"], "abc123")
        self.assertEqual(normalized["priority"], "high")
        self.assertEqual(normalized["targets"], ["mac", "windows"])
        self.assertEqual(normalized["validation"], "smoke")
        self.assertEqual(normalized["status"], "pending")
        self.assertEqual(normalized["provenance"]["execution_kind"], "hosted")
        self.assertEqual(normalized["submission"]["provenance"]["execution_kind"], "hosted")

        preserved = self.mod.normalize_job({"id": "manual-id", "targets": []})
        self.assertEqual(preserved["id"], "manual-id")
        self.assertEqual(preserved["priority"], "normal")
        self.assertEqual(preserved["validation"], "full")
        self.assertEqual(preserved["targets"], [])
        self.assertEqual(preserved["status"], "pending")
        self.assertEqual(preserved["submission"]["provenance"]["execution_kind"], "direct")
        self.assertEqual(preserved["provenance"]["execution_kind"], "direct")

        queue_file = self.mod.queue_path()
        self.assertEqual(self.mod.load_queue_unlocked(), [])

        queue_file.parent.mkdir(parents=True, exist_ok=True)
        queue_file.write_text(json.dumps([legacy_job]) + "\n")
        self.assertEqual(self.mod.load_queue_unlocked()[0]["targets"], ["mac", "windows"])

        queue_file.write_text(json.dumps({"jobs": [legacy_job]}) + "\n")
        self.assertEqual(self.mod.load_queue_unlocked()[0]["priority"], "high")

        self.mod.save_queue_unlocked([normalized])
        self.assertEqual(json.loads(queue_file.read_text())[0]["id"], normalized["id"])
        self.assertTrue(queue_file.read_text().endswith("\n"))

    def test_normalize_job_prefers_top_level_provenance_and_validates_fields(self):
        job = {
            "id": "explicit",
            "priority": "low",
            "targets": ["ubuntu", "ubuntu", "mac"],
            "status": "running",
            "validation": "full",
            "submission": {
                "provenance": {
                    "execution_kind": "hosted",
                    "hosted_orchestrator": "github-actions",
                    "runner_provider": "github-hosted",
                }
            },
            "provenance": {
                "execution_kind": "local",
                "host": "dev-mac",
            },
        }

        normalized = self.mod.normalize_job(job)

        self.assertEqual(normalized["id"], "explicit")
        self.assertEqual(normalized["priority"], "low")
        self.assertEqual(normalized["targets"], ["mac", "ubuntu"])
        self.assertEqual(normalized["status"], "running")
        self.assertEqual(normalized["validation"], "full")
        self.assertEqual(normalized["submission"]["provenance"]["execution_kind"], "hosted")
        self.assertEqual(normalized["provenance"]["execution_kind"], "local")
        self.assertEqual(normalized["provenance"]["host"], "dev-mac")

        with self.assertRaisesRegex(ValueError, "Invalid priority"):
            self.mod.normalize_job({"priority": "urgent"})
        with self.assertRaisesRegex(ValueError, "Invalid validation mode"):
            self.mod.normalize_job({"validation": "quick"})



if __name__ == "__main__":
    unittest.main()
