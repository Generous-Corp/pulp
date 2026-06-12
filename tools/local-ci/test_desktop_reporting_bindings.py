#!/usr/bin/env python3
"""Tests for desktop reporting facade bindings."""

import importlib.util
import types
import unittest
from pathlib import Path


MODULE_PATH = Path(__file__).with_name("desktop_reporting_bindings.py")


def load_module():
    spec = importlib.util.spec_from_file_location("desktop_reporting_bindings_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class DesktopReportingBindingsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def _bindings(self, runner_name: str, runner):
        bindings = {
            "_reporting": types.SimpleNamespace(**{runner_name: runner}),
            "ROOT": Path("/repo"),
        }
        for name in [
            "_run_git",
            "_reset_local_worktree",
            "_clear_directory_contents",
            "git_origin_http_url",
            "create_desktop_publish_bundle",
            "now_iso",
            "atomic_write_text",
            "write_desktop_publish_rollups",
            "publish_report_to_branch",
            "desktop_publish_root",
            "desktop_publish_reports",
            "desktop_artifact_root",
            "desktop_run_manifests",
            "desktop_run_summary",
            "desktop_rollup_dir",
            "desktop_proof_summaries",
        ]:
            bindings[name] = object()
        return bindings

    def test_publish_report_to_branch_binds_dependencies(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return {"mode": "branch"}

        bindings = self._bindings("publish_report_to_branch", runner)
        config = {"desktop_automation": {}}
        report = {"output_dir": "/tmp/report"}

        self.assertEqual(self.mod.publish_report_to_branch(bindings, config, report), {"mode": "branch"})
        self.assertEqual(captured["args"], (config, report))
        self.assertIs(captured["kwargs"]["root"], bindings["ROOT"])
        self.assertIs(captured["kwargs"]["run_git_fn"], bindings["_run_git"])
        self.assertIs(captured["kwargs"]["reset_local_worktree_fn"], bindings["_reset_local_worktree"])
        self.assertIs(captured["kwargs"]["clear_directory_contents_fn"], bindings["_clear_directory_contents"])
        self.assertIs(captured["kwargs"]["git_origin_http_url_fn"], bindings["git_origin_http_url"])

    def test_stage_publish_and_publish_rollups_bind_dependencies(self):
        captured = {}

        def stage_runner(*args, **kwargs):
            captured["stage_args"] = args
            captured["stage_kwargs"] = kwargs
            return {"run_count": 1}

        bindings = self._bindings("stage_desktop_publish_report", stage_runner)
        config = {"desktop_automation": {}}
        manifests = [{"target": "mac"}]
        output_dir = Path("/tmp/out")

        self.assertEqual(
            self.mod.stage_desktop_publish_report(
                bindings,
                config,
                manifests,
                output_dir=output_dir,
                label="gallery",
            ),
            {"run_count": 1},
        )
        self.assertEqual(captured["stage_args"], (config, manifests))
        self.assertIs(captured["stage_kwargs"]["output_dir"], output_dir)
        self.assertEqual(captured["stage_kwargs"]["label"], "gallery")
        for name in [
            "create_desktop_publish_bundle",
            "now_iso",
            "atomic_write_text",
            "write_desktop_publish_rollups",
            "publish_report_to_branch",
        ]:
            self.assertIs(captured["stage_kwargs"][f"{name}_fn"], bindings[name])

        def rollups_runner(*args, **kwargs):
            captured["rollups_args"] = args
            captured["rollups_kwargs"] = kwargs

        bindings = self._bindings("write_desktop_publish_rollups", rollups_runner)
        self.mod.write_desktop_publish_rollups(bindings, config)
        self.assertEqual(captured["rollups_args"], (config,))
        self.assertIs(captured["rollups_kwargs"]["desktop_publish_root_fn"], bindings["desktop_publish_root"])
        self.assertIs(captured["rollups_kwargs"]["desktop_publish_reports_fn"], bindings["desktop_publish_reports"])
        self.assertIs(captured["rollups_kwargs"]["atomic_write_text_fn"], bindings["atomic_write_text"])

    def test_report_listing_and_run_manifest_bind_dependencies(self):
        cases = [
            (
                "desktop_publish_reports",
                self.mod.desktop_publish_reports,
                {"limit": 2},
                {"desktop_publish_root_fn": "desktop_publish_root"},
            ),
            (
                "desktop_run_manifests",
                self.mod.desktop_run_manifests,
                {"target_name": "mac", "action": "smoke"},
                {"desktop_artifact_root_fn": "desktop_artifact_root"},
            ),
        ]
        for runner_name, wrapper, kwargs, expected_bindings in cases:
            with self.subTest(runner_name=runner_name):
                captured = {}

                def runner(*args, **runner_kwargs):
                    captured["args"] = args
                    captured["kwargs"] = runner_kwargs
                    return [{"ok": True}]

                bindings = self._bindings(runner_name, runner)
                config = {"desktop_automation": {}}
                self.assertEqual(wrapper(bindings, config, **kwargs), [{"ok": True}])
                self.assertEqual(captured["args"], (config,))
                for kwarg, binding_name in expected_bindings.items():
                    self.assertIs(captured["kwargs"][kwarg], bindings[binding_name])
                for key, value in kwargs.items():
                    self.assertEqual(captured["kwargs"][key], value)

    def test_proof_rollup_and_prune_bind_dependencies(self):
        captured = {}

        def proof_runner(*args, **kwargs):
            captured["proof_args"] = args
            captured["proof_kwargs"] = kwargs
            return [{"latest_run": {}}]

        bindings = self._bindings("desktop_proof_summaries", proof_runner)
        config = {"desktop_automation": {}}
        result = self.mod.desktop_proof_summaries(
            bindings,
            config,
            target_name="mac",
            action="inspect",
            source_mode="exact-sha",
            sha="abc",
            branch="feature",
            limit=3,
        )
        self.assertEqual(result, [{"latest_run": {}}])
        self.assertEqual(captured["proof_args"], (config,))
        for key, value in {
            "target_name": "mac",
            "action": "inspect",
            "source_mode": "exact-sha",
            "sha": "abc",
            "branch": "feature",
            "limit": 3,
        }.items():
            self.assertEqual(captured["proof_kwargs"][key], value)
        self.assertIs(captured["proof_kwargs"]["desktop_run_manifests_fn"], bindings["desktop_run_manifests"])
        self.assertIs(captured["proof_kwargs"]["desktop_run_summary_fn"], bindings["desktop_run_summary"])

        def run_rollups_runner(*args, **kwargs):
            captured["run_rollups_args"] = args
            captured["run_rollups_kwargs"] = kwargs

        bindings = self._bindings("write_desktop_run_rollups", run_rollups_runner)
        self.mod.write_desktop_run_rollups(bindings, config, target_name="windows")
        self.assertEqual(captured["run_rollups_args"], (config,))
        self.assertEqual(captured["run_rollups_kwargs"]["target_name"], "windows")
        for name in [
            "desktop_rollup_dir",
            "desktop_run_manifests",
            "desktop_run_summary",
            "desktop_proof_summaries",
            "atomic_write_text",
        ]:
            self.assertIs(captured["run_rollups_kwargs"][f"{name}_fn"], bindings[name])

        def prune_runner(*args, **kwargs):
            captured["prune_args"] = args
            captured["prune_kwargs"] = kwargs
            return [Path("/tmp/run")]

        bindings = self._bindings("prune_desktop_run_manifests", prune_runner)
        self.assertEqual(
            self.mod.prune_desktop_run_manifests(
                bindings,
                config,
                target_name="mac",
                older_than_days=7,
                keep_last=2,
            ),
            [Path("/tmp/run")],
        )
        self.assertEqual(captured["prune_args"], (config,))
        self.assertEqual(captured["prune_kwargs"]["target_name"], "mac")
        self.assertEqual(captured["prune_kwargs"]["older_than_days"], 7)
        self.assertEqual(captured["prune_kwargs"]["keep_last"], 2)
        self.assertIs(captured["prune_kwargs"]["desktop_run_manifests_fn"], bindings["desktop_run_manifests"])

    def test_desktop_rollup_dir_binds_artifact_root(self):
        captured = {}

        def runner(*args, **kwargs):
            captured["args"] = args
            captured["kwargs"] = kwargs
            return Path("/tmp/rollups")

        bindings = self._bindings("desktop_rollup_dir", runner)
        config = {"desktop_automation": {}}
        self.assertEqual(self.mod.desktop_rollup_dir(bindings, config, "mac"), Path("/tmp/rollups"))
        self.assertEqual(captured["args"], (config, "mac"))
        self.assertIs(captured["kwargs"]["desktop_artifact_root_fn"], bindings["desktop_artifact_root"])


if __name__ == "__main__":
    unittest.main()
