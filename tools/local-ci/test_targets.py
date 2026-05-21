#!/usr/bin/env python3
"""Tests for the targets resolution helpers.

Split out of test_local_ci.py (roadmap P11-3) so the test surface mirrors
the extracted targets module. The harness still loads the local_ci.py
orchestrator, which re-exports the targets symbols.
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



class TargetsTests(unittest.TestCase):
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

    def test_parse_targets_arg_trims_sorts_deduplicates_and_handles_empty_values(self):
        self.assertIsNone(self.mod.parse_targets_arg(None))
        self.assertIsNone(self.mod.parse_targets_arg(""))
        self.assertIsNone(self.mod.parse_targets_arg("   "))
        self.assertEqual(
            self.mod.parse_targets_arg(" ubuntu,mac,ubuntu,, windows "),
            ["mac", "ubuntu", "windows"],
        )

    def test_resolve_targets_uses_string_defaults_and_enabled_fallback(self):
        config = {
            "targets": {
                "mac": {"enabled": True},
                "ubuntu": {"enabled": True},
                "windows": {"enabled": True},
            },
            "defaults": {"targets": "ubuntu, mac,ubuntu"},
        }

        self.assertEqual(self.mod.resolve_targets(config, None), ["mac", "ubuntu"])
        self.assertEqual(self.mod.resolve_targets(config, []), [])

        config_without_defaults = {
            "targets": {
                "mac": {"enabled": True},
                "ubuntu": {"enabled": False},
                "windows": {},
            },
            "defaults": {},
        }
        self.assertEqual(
            self.mod.resolve_targets(config_without_defaults, None),
            ["mac", "windows"],
        )



if __name__ == "__main__":
    unittest.main()
