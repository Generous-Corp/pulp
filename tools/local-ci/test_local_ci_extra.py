#!/usr/bin/env python3
"""Additional pure-helper coverage for tools/local-ci/local_ci.py."""

from __future__ import annotations

import importlib
import io
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest
from argparse import Namespace
from contextlib import nullcontext, redirect_stdout
from datetime import datetime, timezone
from unittest import mock

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("local_ci.py")


def load_module():
    return load_module_from_path(MODULE_PATH, module_name="pulp_local_ci_extra", add_module_dir=True)


class LocalCiPureHelperTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        # R2-1 (#2645): cloud helpers moved to cloud.py — patch the cloud module.
        self.cloud = importlib.import_module("cloud")
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_desktop_config_dispatch_and_error_paths_report_context(self) -> None:
        with mock.patch.object(self.mod, "load_config", side_effect=FileNotFoundError("missing config")):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_config_show(Namespace(json=False)), 1)
        self.assertIn("missing config", buf.getvalue())

        desktop_config = {
            "desktop_automation": {
                "artifact_root": "runs",
                "publish_mode": "local",
                "publish_branch": "desktop-artifacts",
                "retention_days": 7,
            }
        }
        with mock.patch.object(self.mod, "load_config", return_value=desktop_config):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_config_show(Namespace(json=False)), 0)
        self.assertIn("Desktop automation config:", buf.getvalue())

        with mock.patch.object(self.mod, "load_config", return_value={"desktop_automation": {}}):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_config_set(Namespace(key="retention_days", value="-1", json=False)), 1)
        self.assertIn("retention_days must be >= 0", buf.getvalue())

        with mock.patch.object(self.mod, "load_config", return_value={"desktop_automation": {}}):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_config_set(Namespace(key="target.mac.bad.field", value="1", json=False)), 1)
        self.assertIn("Target desktop config keys must look like", buf.getvalue())

        with mock.patch.object(self.mod, "cmd_desktop_config_show", return_value=7) as show:
            self.assertEqual(self.mod.cmd_desktop_config(Namespace(desktop_config_command="show")), 7)
        show.assert_called_once()

        buf = io.StringIO()
        with redirect_stdout(buf):
            self.assertEqual(self.mod.cmd_desktop_config(Namespace(desktop_config_command=None)), 1)
        self.assertIn("desktop config subcommand required", buf.getvalue())

    def test_desktop_listing_publish_cleanup_empty_and_error_paths(self) -> None:
        with mock.patch.object(self.mod, "load_config", side_effect=FileNotFoundError("missing desktop config")):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_recent(Namespace(target=None, action=None, limit=5, json=False)), 1)
        self.assertIn("missing desktop config", buf.getvalue())

        with mock.patch.object(self.mod, "load_config", return_value={"desktop_automation": {}}), \
             mock.patch.object(self.mod, "desktop_run_manifests", return_value=[]):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_recent(Namespace(target=None, action=None, limit=5, json=True)), 0)
        self.assertIn("No desktop automation runs found.", buf.getvalue())

        with mock.patch.object(self.mod, "load_config", return_value={"desktop_automation": {}}), \
             mock.patch.object(self.mod, "desktop_proof_summaries", side_effect=ValueError("bad source")):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_proof(Namespace(target=None, action=None, source_mode="bad", sha=None, branch=None, limit=5, json=False)), 1)
        self.assertIn("bad source", buf.getvalue())

        with mock.patch.object(self.mod, "load_config", return_value={"desktop_automation": {}}), \
             mock.patch.object(self.mod, "desktop_proof_summaries", return_value=[]):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_proof(Namespace(target="mac", action="smoke", source_mode="live", sha="a" * 40, branch="feature/a", limit=5, json=False)), 0)
        self.assertIn("No desktop proofs found", buf.getvalue())
        self.assertIn("sha=aaaaaaaaaaaa", buf.getvalue())

        with mock.patch.object(self.mod, "load_config", return_value={"desktop_automation": {}}), \
             mock.patch.object(self.mod, "desktop_run_manifests", return_value=[]):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_publish(Namespace(target=None, action=None, limit=5, output=None, label=None, json=False)), 0)
        self.assertIn("No desktop automation runs found.", buf.getvalue())

        with mock.patch.object(self.mod, "load_config", return_value={"desktop_automation": {}}), \
             mock.patch.object(self.mod, "desktop_run_manifests", return_value=[{"target": "mac"}]), \
             mock.patch.object(self.mod, "stage_desktop_publish_report", side_effect=RuntimeError("publish failed")):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_publish(Namespace(target=None, action=None, limit=5, output=None, label=None, json=False)), 1)
        self.assertIn("publish failed", buf.getvalue())

        with mock.patch.object(self.mod, "load_config", return_value={"desktop_automation": {"retention_days": 30}}), \
             mock.patch.object(self.mod, "prune_desktop_run_manifests", return_value=[]):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_cleanup(Namespace(target=None, older_than_days=None, keep_last=1, json=False)), 0)
        self.assertIn("Desktop cleanup: nothing to remove.", buf.getvalue())

    def test_desktop_artifact_report_helpers_cover_filter_and_fallback_edges(self) -> None:
        config = {"desktop_automation": {"artifact_root": str(self.root / "artifacts"), "publish_root": str(self.root / "publish")}}
        publish_root = self.mod.desktop_publish_root(config)
        old_dir = publish_root / "2026-01-01T00-00-00Z"
        new_dir = publish_root / "2026-01-02T00-00-00Z"
        malformed_dir = publish_root / "malformed"
        missing_dir = publish_root / "missing-index"
        old_dir.mkdir(parents=True)
        new_dir.mkdir(parents=True)
        malformed_dir.mkdir(parents=True)
        missing_dir.mkdir(parents=True)
        (old_dir / "index.json").write_text(json.dumps({"generated_at": "2026-01-01T00:00:00Z", "label": "old"}))
        (new_dir / "index.json").write_text(json.dumps({"generated_at": "2026-01-02T00:00:00Z", "label": "new"}))
        (malformed_dir / "index.json").write_text("{")

        reports = self.mod.desktop_publish_reports(config)
        self.assertEqual([report["label"] for report in reports], ["new", "old"])
        self.assertEqual(reports[0]["output_dir"], str(new_dir))
        self.assertEqual(reports[0]["index_json"], str(new_dir / "index.json"))
        self.assertEqual(reports[0]["index_html"], str(new_dir / "index.html"))
        self.assertEqual(self.mod.desktop_publish_reports(config, limit=1)[0]["label"], "new")

        self.mod.write_desktop_publish_rollups(config)
        self.assertEqual(json.loads((publish_root / "latest-report.json").read_text())["label"], "new")
        self.assertEqual(len((publish_root / "reports.jsonl").read_text().splitlines()), 2)

        ready = self.root / "ready.txt"
        ready.write_text("ok")
        self.assertEqual(self.mod.wait_for_path(ready, 0.1), ready)
        with self.assertRaisesRegex(RuntimeError, "timed out waiting for artifact"):
            self.mod.wait_for_path(self.root / "missing.txt", 0.0)

        self.assertEqual(self.mod.count_view_tree_nodes("not-a-node"), 0)
        self.assertEqual(self.mod.count_view_tree_nodes({"children": "bad"}), 1)
        self.assertEqual(self.mod.count_view_tree_nodes({"children": [{"children": [{}]}, {}]}), 4)

        app_binary = self.root / "Demo.app" / "Contents" / "MacOS" / "Demo"
        app_binary.parent.mkdir(parents=True)
        app_binary.write_text("")
        self.assertIsNone(self.mod.detect_macos_app_bundle(None))
        self.assertIsNone(self.mod.detect_macos_app_bundle(""))
        self.assertEqual(self.mod.detect_macos_app_bundle(str(app_binary)), self.root / "Demo.app")
        self.assertIsNone(self.mod.macos_bundle_id_for_app_path(self.root / "Missing.app"))
        info_plist = self.root / "Demo.app" / "Contents" / "Info.plist"
        info_plist.write_text("not plist")
        self.assertIsNone(self.mod.macos_bundle_id_for_app_path(self.root / "Demo.app"))
        info_plist.write_bytes(self.mod.plistlib.dumps({"CFBundleIdentifier": "com.example.demo"}))
        self.assertEqual(self.mod.macos_bundle_id_for_app_path(self.root / "Demo.app"), "com.example.demo")
        info_plist.write_bytes(self.mod.plistlib.dumps({"CFBundleIdentifier": ""}))
        self.assertIsNone(self.mod.macos_bundle_id_for_app_path(self.root / "Demo.app"))

    def test_desktop_manifest_and_window_wait_helpers_cover_edge_paths(self) -> None:
        artifact_root = self.root / "artifacts"
        config = {
            "desktop_automation": {
                "artifact_root": str(artifact_root),
                "targets": {"mac": {"adapter": "macos-local"}, "windows": {"adapter": "windows-session-agent"}},
            }
        }
        valid_bundle = artifact_root / "mac" / "smoke" / "run-new"
        old_bundle = artifact_root / "mac" / "smoke" / "run-old"
        malformed_bundle = artifact_root / "mac" / "smoke" / "bad"
        wrong_action = artifact_root / "mac" / "inspect" / "run"
        missing_manifest = artifact_root / "windows" / "smoke" / "run"
        for path in (valid_bundle, old_bundle, malformed_bundle, wrong_action, missing_manifest):
            path.mkdir(parents=True)
        (valid_bundle / "manifest.json").write_text(json.dumps({"target": "mac", "started_at": "2026-01-02T00:00:00Z"}))
        (old_bundle / "manifest.json").write_text(json.dumps({"target": "mac", "completed_at": "2026-01-01T00:00:00Z"}))
        (malformed_bundle / "manifest.json").write_text("{")
        (wrong_action / "manifest.json").write_text(json.dumps({"target": "mac", "started_at": "2026-01-03T00:00:00Z"}))

        manifests = self.mod.desktop_run_manifests(config, target_name="mac", action="smoke")
        self.assertEqual(len(manifests), 2)
        self.assertEqual(manifests[0]["target"], "mac")
        self.assertEqual(manifests[0]["artifacts"]["bundle_dir"], str(valid_bundle))
        self.assertEqual(manifests[1]["artifacts"]["bundle_dir"], str(old_bundle))
        self.assertEqual(self.mod.desktop_run_manifests(config, target_name="missing"), [])
        self.assertEqual(len(self.mod.desktop_run_manifests(config, action="inspect")), 1)

        self.assertEqual(self.mod.normalize_desktop_proof_source_mode(None), "legacy")
        self.assertEqual(self.mod.normalize_desktop_proof_source_mode(" exact_sha "), "exact-sha")
        with self.assertRaisesRegex(ValueError, "Invalid desktop proof source mode"):
            self.mod.normalize_desktop_proof_source_mode("archive")
        self.assertEqual(self.mod.desktop_manifest_adapter(config, {"adapter": "custom"}), "custom")
        self.assertEqual(self.mod.desktop_manifest_adapter(config, {"target": "mac"}), "macos-local")
        self.assertEqual(self.mod.desktop_manifest_adapter(config, {"target": "missing"}), "unknown")
        self.assertEqual(self.mod.desktop_manifest_adapter({"desktop_automation": {"targets": []}}, {"target": "mac"}), "unknown")

        with mock.patch.object(self.mod.time, "time", side_effect=[0.0, 0.0, 1.0]), \
             mock.patch.object(self.mod.time, "sleep") as sleep, \
             mock.patch.object(self.mod, "macos_window_info_for_pid", side_effect=subprocess.SubprocessError("boom")):
            with self.assertRaisesRegex(RuntimeError, "boom"):
                self.mod.wait_for_macos_window(123, 0.5)
        sleep.assert_called_once_with(0.2)

        with mock.patch.object(self.mod.time, "time", side_effect=[0.0, 0.0, 1.0]), \
             mock.patch.object(self.mod.time, "sleep") as sleep, \
             mock.patch.object(self.mod, "macos_window_info_for_bundle_id", return_value={"pid": 456, "windows": []}), \
             mock.patch.object(self.mod, "activate_macos_bundle_id", return_value={"stderr": "still hidden"}) as activate:
            with self.assertRaisesRegex(RuntimeError, "still hidden"):
                self.mod.wait_for_macos_bundle_window("com.example.demo", 0.5)
        activate.assert_called_once_with("com.example.demo")
        sleep.assert_called_once_with(0.2)

        with mock.patch.object(self.mod.time, "time", side_effect=[0.0, 0.0]), \
             mock.patch.object(self.mod, "macos_window_info_for_pid", return_value={"windows": [{"id": 7}]}):
            self.assertEqual(self.mod.wait_for_macos_window(123, 0.5), {"id": 7})
        with mock.patch.object(self.mod.time, "time", side_effect=[0.0, 0.0]), \
             mock.patch.object(self.mod, "macos_window_info_for_bundle_id", return_value={"pid": 456, "windows": [{"id": 8}]}):
            self.assertEqual(self.mod.wait_for_macos_bundle_window("com.example.demo", 0.5), (456, {"id": 8}))

    def test_desktop_action_validation_errors_and_dispatch_guards(self) -> None:
        config = {
            "desktop_automation": {
                "targets": {
                    "mac": {"adapter": "macos-local", "target_type": "local"},
                    "linux": {"adapter": "linux-xvfb", "target_type": "ssh"},
                    "windows": {"adapter": "windows-session-agent", "target_type": "ssh"},
                    "other": {"adapter": "remote-session-agent", "target_type": "ssh"},
                }
            }
        }
        with mock.patch.object(self.mod, "load_config", return_value=config), \
             mock.patch.object(self.mod, "resolve_desktop_target", side_effect=lambda _config, name: config["desktop_automation"]["targets"][name]), \
             mock.patch.object(self.mod, "make_desktop_source_request", return_value={}):
            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_smoke(Namespace(target="linux", launch_command=None, bundle_id=None, label=None, output=None, capture_ui_snapshot=False, click=None, click_view_id=None, click_view_type=None, click_view_text=None, click_view_label=None, capture_before=False, settle_secs=0.0, timeout=1.0, pulp_app_automation=False)), 1)
            self.assertIn("requires --command for linux-xvfb", buf.getvalue())

            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_smoke(Namespace(target="windows", launch_command="app", bundle_id=None, label=None, output=None, capture_ui_snapshot=True, click=None, click_view_id=None, click_view_type=None, click_view_text=None, click_view_label=None, capture_before=False, settle_secs=0.0, timeout=1.0, pulp_app_automation=False)), 1)
            self.assertIn("supports --capture-ui-snapshot only with --pulp-app-automation", buf.getvalue())

            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_click(Namespace(target="windows", launch_command="app", bundle_id=None, label=None, output=None, capture_ui_snapshot=False, click=None, click_view_id="root", click_view_type=None, click_view_text=None, click_view_label=None, settle_secs=0.0, timeout=1.0, pulp_app_automation=False)), 1)
            self.assertIn("supports view-target selectors only with --pulp-app-automation", buf.getvalue())

            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop_inspect(Namespace(target="other", launch_command="app", bundle_id=None, label=None, output=None, timeout=1.0, pulp_app_automation=False)), 1)
            self.assertIn("desktop inspect is not implemented", buf.getvalue())

            buf = io.StringIO()
            with redirect_stdout(buf):
                self.assertEqual(self.mod.cmd_desktop(Namespace(desktop_command=None)), 1)
            self.assertIn("desktop subcommand required", buf.getvalue())


if __name__ == "__main__":
    unittest.main(verbosity=2)
