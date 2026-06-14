#!/usr/bin/env python3
"""No-network tests for target config preflight helpers."""

from __future__ import annotations

import json
import pathlib
import tempfile
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = pathlib.Path(__file__).with_name("target_config_preflight.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class TargetConfigPreflightTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_config_source_name_prefers_env_override_then_shared_state(self) -> None:
        shared = self.root / "shared.json"
        worktree = self.root / "worktree.json"

        self.assertEqual(
            self.mod.config_source_name(shared, environ={}, shared_config_path_fn=lambda: shared),
            "shared-state",
        )
        self.assertEqual(
            self.mod.config_source_name(worktree, environ={}, shared_config_path_fn=lambda: shared),
            "worktree-local",
        )
        self.assertEqual(
            self.mod.config_source_name(
                worktree,
                environ={"PULP_LOCAL_CI_CONFIG": str(worktree)},
                shared_config_path_fn=lambda: shared,
            ),
            "env-override",
        )

    def test_config_material_keeps_only_submission_relevant_fields(self) -> None:
        material = self.mod.config_material_for_targets(
            {
                "targets": {
                    "ubuntu": {
                        "type": "ssh",
                        "enabled": False,
                        "host": "ubuntu",
                        "repo_path": "/repo",
                        "ignored": "value",
                        "cmake_generator": "",
                    },
                    "missing": {},
                }
            },
            ["ubuntu", "mac"],
        )

        self.assertEqual(
            material,
            {"ubuntu": {"type": "ssh", "enabled": False, "host": "ubuntu", "repo_path": "/repo"}},
        )

    def test_find_material_config_drift_compares_shared_and_worktree_config(self) -> None:
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


if __name__ == "__main__":
    unittest.main()
