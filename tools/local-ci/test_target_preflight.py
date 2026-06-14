#!/usr/bin/env python3
"""No-network tests for local-ci target preflight helpers."""

from __future__ import annotations

import json
import pathlib
import tempfile
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("target_preflight.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class TargetPreflightTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_config_material_source_and_drift(self) -> None:
        shared = self.root / "shared.json"
        worktree = self.root / "worktree.json"
        shared.write_text(
            json.dumps({"targets": {"ubuntu": {"type": "ssh", "host": "shared", "repo_path": "/repo"}}})
        )
        worktree.write_text(
            json.dumps({"targets": {"ubuntu": {"type": "ssh", "host": "worktree", "repo_path": "/repo"}}})
        )

        drift = self.mod.find_material_config_drift(
            ["ubuntu"],
            shared_config_path_fn=lambda: shared,
            worktree_config_path_fn=lambda: worktree,
            config_material_for_targets_fn=self.mod.config_material_for_targets,
        )
        self.assertEqual(len(drift), 1)
        self.assertIn("shared", drift[0])
        self.assertIn("worktree", drift[0])
        self.assertEqual(
            self.mod.config_source_name(shared, environ={}, shared_config_path_fn=lambda: shared),
            "shared-state",
        )
        self.assertEqual(
            self.mod.config_source_name(worktree, environ={"PULP_LOCAL_CI_CONFIG": str(worktree)}, shared_config_path_fn=lambda: shared),
            "env-override",
        )

        worktree.write_text("{broken")
        self.assertEqual(
            self.mod.find_material_config_drift(
                ["ubuntu"],
                shared_config_path_fn=lambda: shared,
                worktree_config_path_fn=lambda: worktree,
                config_material_for_targets_fn=self.mod.config_material_for_targets,
            ),
            [],
        )

    def test_submission_metadata_records_warnings_and_namespace_failover(self) -> None:
        config_path = self.root / "config.json"
        config_path.write_text("{}\n")
        config = {
            "targets": {
                "mac": {"type": "local", "enabled": True},
                "windows": {
                    "type": "ssh",
                    "enabled": True,
                    "host": "win2",
                    "fallback_host": "win",
                    "repo_path": "C:\\Pulp",
                },
            },
            "defaults": {},
        }

        def fallback_state(name, target_cfg, defaults):
            return self.mod.preflight_target_host_state(
                name,
                target_cfg,
                defaults,
                ssh_reachable_fn=lambda host, _timeout: host == "win",
            )

        metadata = self.mod.build_submission_metadata(
            config,
            "feature/topic",
            "a" * 40,
            ["mac", "windows"],
            "normal",
            "full",
            allow_root_mismatch=False,
            allow_unreachable_targets=False,
            root=self.root.resolve(),
            cwd_fn=lambda: self.root,
            git_root_for_fn=lambda _cwd: self.root.resolve(),
            config_path_fn=lambda: config_path,
            config_source_name_fn=lambda _path: "worktree-local",
            preflight_target_host_state_fn=fallback_state,
            find_material_config_drift_fn=lambda _targets: ["windows: drift"],
            normalize_provenance_fn=lambda: {"execution_kind": "direct"},
            environ={},
        )
        self.assertEqual(metadata["target_hosts"]["mac"]["status"], "local")
        self.assertEqual(metadata["target_hosts"]["windows"]["status"], "fallback-up")
        self.assertEqual(metadata["target_hosts"]["windows"]["resolved_host"], "win")
        self.assertEqual(metadata["config_drift"], ["windows: drift"])
        self.assertEqual(metadata["provenance"], {"execution_kind": "direct"})
        self.assertTrue(any("fallback win is up" in warning for warning in metadata["warnings"]))
        self.assertTrue(any("config drift detected" in warning for warning in metadata["warnings"]))

        unreachable_config = {
            "targets": {"ubuntu": {"type": "ssh", "host": "ubuntu-primary", "repo_path": "/repo"}},
            "defaults": {},
            "github_actions": {"defaults": {"provider": "github-hosted"}},
        }

        def unreachable_state(name, target_cfg, defaults):
            return self.mod.preflight_target_host_state(
                name,
                target_cfg,
                defaults,
                ssh_reachable_fn=lambda _host, _timeout: False,
            )

        with self.assertRaisesRegex(ValueError, "Pass --allow-unreachable-targets"):
            self.mod.build_submission_metadata(
                unreachable_config,
                "feature/topic",
                "b" * 40,
                ["ubuntu"],
                "normal",
                "full",
                allow_root_mismatch=False,
                allow_unreachable_targets=False,
                root=self.root.resolve(),
                cwd_fn=lambda: self.root,
                git_root_for_fn=lambda _cwd: self.root.resolve(),
                config_path_fn=lambda: config_path,
                config_source_name_fn=lambda _path: "worktree-local",
                preflight_target_host_state_fn=unreachable_state,
                find_material_config_drift_fn=lambda _targets: [],
                normalize_provenance_fn=lambda: {"execution_kind": "direct"},
                environ={},
            )

        namespace_config = dict(unreachable_config)
        namespace_config["github_actions"] = {"defaults": {"provider": "namespace"}}
        failed_over = self.mod.build_submission_metadata(
            namespace_config,
            "feature/topic",
            "b" * 40,
            ["ubuntu"],
            "normal",
            "full",
            allow_root_mismatch=False,
            allow_unreachable_targets=False,
            root=self.root.resolve(),
            cwd_fn=lambda: self.root,
            git_root_for_fn=lambda _cwd: self.root.resolve(),
            config_path_fn=lambda: config_path,
            config_source_name_fn=lambda _path: "worktree-local",
            preflight_target_host_state_fn=unreachable_state,
            find_material_config_drift_fn=lambda _targets: [],
            normalize_provenance_fn=lambda: {"execution_kind": "direct"},
            environ={},
        )
        self.assertEqual(failed_over["target_hosts"]["ubuntu"]["status"], "namespace-failover")
        self.assertEqual(failed_over["namespace_failover_targets"], ["ubuntu"])
        self.assertNotIn("error", failed_over["target_hosts"]["ubuntu"])


if __name__ == "__main__":
    unittest.main()
