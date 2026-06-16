#!/usr/bin/env python3
"""Tests for split desktop report command modules."""

from __future__ import annotations

from argparse import Namespace
from pathlib import Path
import json
import unittest

from module_test_utils import load_local_ci_module


def load_module(name: str):
    return load_local_ci_module(f"{name}.py", add_module_dir=True)


class DesktopReportCommandModuleTests(unittest.TestCase):
    def test_recent_and_publish_empty_run_states_match_facade_behavior(self) -> None:
        recent = load_module("desktop_report_recent_commands_cli")
        publish = load_module("desktop_report_publish_commands_cli")
        config = {"desktop_automation": {"retention_days": 30}}

        recent_lines: list[str] = []
        self.assertEqual(
            recent.cmd_desktop_recent(
                Namespace(target=None, action=None, limit=5, json=False),
                load_config_fn=lambda: config,
                desktop_run_manifests_fn=lambda *_args, **_kwargs: [],
                desktop_run_summary_fn=lambda _config, manifest: manifest,
                desktop_recent_lines_fn=lambda *_args, **_kwargs: [],
                short_sha_fn=lambda value: value[:12],
                print_fn=recent_lines.append,
            ),
            0,
        )

        publish_lines: list[str] = []
        self.assertEqual(
            publish.cmd_desktop_publish(
                Namespace(target=None, action=None, limit=5, output=None, label=None, json=False),
                load_config_fn=lambda: config,
                desktop_run_manifests_fn=lambda *_args, **_kwargs: [],
                stage_desktop_publish_report_fn=lambda *_args, **_kwargs: {},
                desktop_publish_lines_fn=lambda _report: [],
                print_fn=publish_lines.append,
            ),
            0,
        )

        self.assertEqual(recent_lines, ["No desktop automation runs found."])
        self.assertEqual(publish_lines, ["No desktop automation runs found."])

    def test_proof_empty_state_and_cleanup_json_output(self) -> None:
        proof = load_module("desktop_report_proof_commands_cli")
        cleanup = load_module("desktop_report_cleanup_commands_cli")
        config = {"desktop_automation": {"retention_days": 30}}

        proof_lines: list[str] = []
        self.assertEqual(
            proof.cmd_desktop_proof(
                Namespace(target="mac", action="smoke", source_mode="legacy", sha=None, branch=None, limit=5, json=False),
                load_config_fn=lambda: config,
                desktop_proof_summaries_fn=lambda *_args, **_kwargs: [],
                desktop_proof_empty_line_fn=lambda **_kwargs: "No desktop proofs found.",
                desktop_proof_lines_fn=lambda *_args, **_kwargs: [],
                short_sha_fn=lambda value: value[:12],
                print_fn=proof_lines.append,
            ),
            0,
        )

        removed_path = Path("/tmp/pulp-local-ci-run")
        removed: list[Path] = []
        rollups: list[tuple[tuple, dict]] = []
        cleanup_lines: list[str] = []
        self.assertEqual(
            cleanup.cmd_desktop_cleanup(
                Namespace(target="mac", older_than_days=None, keep_last=1, json=True),
                load_config_fn=lambda: config,
                prune_desktop_run_manifests_fn=lambda *_args, **_kwargs: [removed_path],
                write_desktop_run_rollups_fn=lambda *args, **kwargs: rollups.append((args, kwargs)),
                desktop_cleanup_empty_line_fn=lambda: "none",
                desktop_cleanup_lines_fn=lambda paths: [f"removed {len(paths)}"],
                remove_tree_fn=lambda path, **_kwargs: removed.append(path),
                print_fn=cleanup_lines.append,
            ),
            0,
        )

        self.assertEqual(proof_lines, ["No desktop proofs found."])
        self.assertEqual(removed, [removed_path])
        self.assertEqual(len(rollups), 2)
        self.assertEqual(json.loads(cleanup_lines[-1]), {"removed": [str(removed_path)]})


if __name__ == "__main__":
    unittest.main()
