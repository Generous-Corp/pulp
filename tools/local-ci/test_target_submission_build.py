#!/usr/bin/env python3
"""No-network tests for target submission metadata construction."""

from __future__ import annotations

import pathlib
import tempfile
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("target_submission_build.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class TargetSubmissionBuildTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)
        self.config_path = self.root / "config.json"
        self.config_path.write_text("{}\n")

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def build_metadata(self, config: dict, targets: list[str], **overrides) -> dict:
        kwargs = {
            "allow_root_mismatch": False,
            "allow_unreachable_targets": False,
            "root": self.root.resolve(),
            "cwd_fn": lambda: self.root,
            "git_root_for_fn": lambda _cwd: self.root.resolve(),
            "config_path_fn": lambda: self.config_path,
            "config_source_name_fn": lambda _path: "worktree-local",
            "preflight_target_host_state_fn": lambda name, _target_cfg, _defaults: {"status": "local", "name": name},
            "find_material_config_drift_fn": lambda _targets: [],
            "normalize_provenance_fn": lambda: {"execution_kind": "direct"},
            "environ": {},
        }
        kwargs.update(overrides)
        return self.mod.build_submission_metadata(
            config,
            "feature/topic",
            "a" * 40,
            targets,
            "normal",
            "full",
            **kwargs,
        )

    def test_records_target_preflight_warnings_drift_and_provenance(self) -> None:
        config = {
            "targets": {
                "mac": {"type": "local"},
                "windows": {"type": "ssh", "host": "win2", "repo_path": "C:\\Pulp"},
            },
            "defaults": {},
        }

        def preflight_state(name, _target_cfg, _defaults):
            if name == "windows":
                return {"status": "fallback-up", "warning": "windows: fallback win is up"}
            return {"status": "local"}

        metadata = self.build_metadata(
            config,
            ["mac", "windows"],
            preflight_target_host_state_fn=preflight_state,
            find_material_config_drift_fn=lambda _targets: ["windows: drift"],
        )

        self.assertEqual(metadata["target_hosts"]["mac"]["status"], "local")
        self.assertEqual(metadata["target_hosts"]["windows"]["status"], "fallback-up")
        self.assertEqual(metadata["config_drift"], ["windows: drift"])
        self.assertEqual(metadata["provenance"], {"execution_kind": "direct"})
        self.assertTrue(any("fallback win is up" in warning for warning in metadata["warnings"]))
        self.assertTrue(any("config drift detected" in warning for warning in metadata["warnings"]))

    def test_rejects_root_mismatch_unless_allowed(self) -> None:
        other_root = self.root / "other"
        other_root.mkdir()

        with self.assertRaisesRegex(ValueError, "different git root"):
            self.build_metadata({}, [], git_root_for_fn=lambda _cwd: other_root.resolve())

        metadata = self.build_metadata(
            {},
            [],
            git_root_for_fn=lambda _cwd: other_root.resolve(),
            allow_root_mismatch=True,
        )
        self.assertEqual(metadata["cwd_git_root"], str(other_root.resolve()))

    def test_unreachable_targets_fail_or_namespace_failover(self) -> None:
        unreachable_config = {
            "targets": {"ubuntu": {"type": "ssh", "host": "ubuntu-primary", "repo_path": "/repo"}},
            "defaults": {},
            "github_actions": {"defaults": {"provider": "github-hosted"}},
        }

        def unreachable_state(name, _target_cfg, _defaults):
            return {"status": "unreachable", "error": f"{name}: SSH host unreachable"}

        with self.assertRaisesRegex(ValueError, "Pass --allow-unreachable-targets"):
            self.build_metadata(
                unreachable_config,
                ["ubuntu"],
                preflight_target_host_state_fn=unreachable_state,
            )

        allowed = self.build_metadata(
            unreachable_config,
            ["ubuntu"],
            preflight_target_host_state_fn=unreachable_state,
            allow_unreachable_targets=True,
        )
        self.assertEqual(allowed["target_hosts"]["ubuntu"]["status"], "unreachable")

        namespace_config = dict(unreachable_config)
        namespace_config["github_actions"] = {"defaults": {"provider": "namespace"}}
        failed_over = self.build_metadata(
            namespace_config,
            ["ubuntu"],
            preflight_target_host_state_fn=unreachable_state,
        )
        self.assertEqual(failed_over["target_hosts"]["ubuntu"]["status"], "namespace-failover")
        self.assertEqual(failed_over["namespace_failover_targets"], ["ubuntu"])
        self.assertNotIn("error", failed_over["target_hosts"]["ubuntu"])

    def test_env_override_suppresses_config_drift_lookup(self) -> None:
        called = []
        metadata = self.build_metadata(
            {},
            [],
            find_material_config_drift_fn=lambda targets: called.append(targets) or ["drift"],
            environ={"PULP_LOCAL_CI_CONFIG": str(self.config_path)},
        )

        self.assertEqual(metadata["config_drift"], [])
        self.assertEqual(called, [])


if __name__ == "__main__":
    unittest.main()
