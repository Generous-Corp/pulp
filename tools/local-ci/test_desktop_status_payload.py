#!/usr/bin/env python3
from __future__ import annotations

import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_status_payload.py")


class DesktopStatusPayloadTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def config(self):
        return {
            "desktop_automation": {
                "artifact_root": "runs",
                "publish_mode": "local",
                "publish_branch": "desktop-artifacts",
                "retention_days": 7,
                "targets": {
                    "windows": {
                        "enabled": False,
                        "adapter": "windows-session-agent",
                        "bootstrap": "ssh",
                        "target_type": "remote",
                        "host": "win.example",
                        "repo_path": "C:/src/pulp",
                        "capability_tier": "smoke",
                    },
                    "mac": {
                        "enabled": True,
                        "adapter": "macos-local",
                        "bootstrap": "launchagent",
                        "target_type": "local",
                        "capability_tier": "full",
                        "optional": {"webview_driver": True},
                    },
                },
            }
        }

    def latest_run(self):
        return {
            "label": "run",
            "completed_at": "2026-06-12T12:00:00Z",
            "interaction_mode": "pulp-app",
            "run_status": "pass",
            "source": {"mode": "branch", "branch": "feature", "sha": "a" * 40},
            "proof_scope": "branch",
            "host": "macstudio",
            "artifacts": {
                "screenshot": "after.png",
                "before_screenshot": "before.png",
                "diff_screenshot": "diff.png",
                "image_change": {"ratio": 0.01},
                "ui_snapshot": "ui.json",
                "bundle_dir": "bundle",
            },
        }

    def test_status_payload_builds_sorted_targets_and_latest_fields(self):
        config = self.config()
        latest_run = self.latest_run()
        proof = {"label": "proof"}
        publish = {"label": "publish"}

        payload = self.mod.desktop_status_payload(
            config,
            target_name=None,
            desktop_receipt_for_fn=lambda name: {"installed_at": "then", "contract": {"name": name}} if name == "mac" else {},
            desktop_capabilities_for_fn=lambda adapter, tier, _optional=None: [adapter, tier],
            desktop_optional_capabilities_fn=lambda optional: ["webview_dom"] if optional else [],
            desktop_run_manifests_fn=lambda _config, **kwargs: [{"label": kwargs["target_name"]}] if kwargs["target_name"] == "mac" else [],
            desktop_run_summary_fn=lambda _config, _manifest: latest_run,
            desktop_proof_summaries_fn=lambda _config, **kwargs: [proof] if kwargs["target_name"] == "mac" else [],
            normalize_desktop_optional_config_fn=lambda optional: dict(optional or {}),
            desktop_target_contract_fn=lambda name, _target: {"fallback": name},
            desktop_publish_reports_fn=lambda _config, **_kwargs: [publish],
        )

        self.assertEqual(payload["artifact_root"], "runs")
        self.assertEqual(payload["latest_publish"], publish)
        self.assertEqual([target["name"] for target in payload["targets"]], ["mac", "windows"])
        mac = payload["targets"][0]
        self.assertTrue(mac["installed"])
        self.assertEqual(mac["capabilities_text"], "macos-local, full")
        self.assertEqual(mac["optional_features"], {"webview_driver": True})
        self.assertEqual(mac["optional_capabilities"], ["webview_dom"])
        self.assertEqual(mac["latest_proof"], proof)
        self.assertEqual(mac["latest_run"]["label"], "run")
        self.assertEqual(mac["latest_run"]["source_sha"], "a" * 40)
        self.assertEqual(mac["latest_run"]["diff_screenshot"], "diff.png")
        windows = payload["targets"][1]
        self.assertFalse(windows["installed"])
        self.assertEqual(windows["contract"], {"fallback": "windows"})
        self.assertIsNone(windows["latest_run"])
        self.assertIsNone(windows["latest_proof"])

    def test_status_payload_filters_target_and_reports_unknown_target(self):
        config = self.config()
        common = {
            "desktop_receipt_for_fn": lambda _name: {},
            "desktop_capabilities_for_fn": lambda *_args: [],
            "desktop_optional_capabilities_fn": lambda _optional: [],
            "desktop_run_manifests_fn": lambda *_args, **_kwargs: [],
            "desktop_run_summary_fn": lambda _config, _manifest: {},
            "desktop_proof_summaries_fn": lambda *_args, **_kwargs: [],
            "normalize_desktop_optional_config_fn": lambda optional: dict(optional or {}),
            "desktop_target_contract_fn": lambda name, _target: {"name": name},
            "desktop_publish_reports_fn": lambda *_args, **_kwargs: [],
        }

        payload = self.mod.desktop_status_payload(config, target_name="windows", **common)
        self.assertEqual([target["name"] for target in payload["targets"]], ["windows"])

        with self.assertRaisesRegex(ValueError, "unknown desktop target `linux`"):
            self.mod.desktop_status_payload(config, target_name="linux", **common)

    def test_latest_run_payload_allows_missing_run(self):
        self.assertIsNone(self.mod.desktop_latest_run_payload(None))


if __name__ == "__main__":
    unittest.main()
