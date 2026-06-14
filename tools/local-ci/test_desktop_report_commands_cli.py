#!/usr/bin/env python3
from __future__ import annotations

import json
from argparse import Namespace
from pathlib import Path
import tempfile
import unittest

from module_test_utils import load_module_from_path


MODULE_PATH = Path(__file__).resolve().with_name("desktop_report_commands_cli.py")


def load_module():
    return load_module_from_path(MODULE_PATH)


class DesktopReportCommandsCliTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.printed: list[str] = []

    def print_line(self, line: str):
        self.printed.append(line)

    def test_recent_proof_publish_and_cleanup_paths(self):
        config = {"desktop_automation": {"retention_days": 30}}
        run_manifest = {"label": "run", "target": "mac"}

        result = self.mod.cmd_desktop_recent(
            Namespace(target="mac", action="smoke", limit=1, json=True),
            load_config_fn=lambda: config,
            desktop_run_manifests_fn=lambda *_args, **_kwargs: [run_manifest],
            desktop_run_summary_fn=lambda _config, manifest: {"label": manifest["label"]},
            desktop_recent_lines_fn=lambda summaries, **_kwargs: [f"recent {summaries[0]['label']}"],
            short_sha_fn=lambda value: value[:12],
            print_fn=self.print_line,
        )
        self.assertEqual(result, 0)
        self.assertEqual(json.loads(self.printed[-1])["runs"], [run_manifest])

        result = self.mod.cmd_desktop_proof(
            Namespace(target="mac", action="smoke", source_mode="legacy", sha=None, branch=None, limit=5, json=False),
            load_config_fn=lambda: config,
            desktop_proof_summaries_fn=lambda *_args, **_kwargs: [],
            desktop_proof_empty_line_fn=lambda **_kwargs: "No desktop proofs found.",
            desktop_proof_lines_fn=lambda *_args, **_kwargs: ["unused"],
            short_sha_fn=lambda value: value[:12],
            print_fn=self.print_line,
        )
        self.assertEqual(result, 0)
        self.assertEqual(self.printed[-1], "No desktop proofs found.")

        result = self.mod.cmd_desktop_publish(
            Namespace(target="mac", action="smoke", limit=1, output=None, label="gallery", json=False),
            load_config_fn=lambda: config,
            desktop_run_manifests_fn=lambda *_args, **_kwargs: [run_manifest],
            stage_desktop_publish_report_fn=lambda *_args, **_kwargs: {"run_count": 1},
            desktop_publish_lines_fn=lambda report: [f"published {report['run_count']}"],
            print_fn=self.print_line,
        )
        self.assertEqual(result, 0)
        self.assertEqual(self.printed[-1], "published 1")

        removed = []
        rollups = []
        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "run"
            path.mkdir()
            result = self.mod.cmd_desktop_cleanup(
                Namespace(target="mac", older_than_days=None, keep_last=1, json=True),
                load_config_fn=lambda: config,
                prune_desktop_run_manifests_fn=lambda *_args, **_kwargs: [path],
                write_desktop_run_rollups_fn=lambda *args, **kwargs: rollups.append((args, kwargs)),
                desktop_cleanup_empty_line_fn=lambda: "none",
                desktop_cleanup_lines_fn=lambda paths: [f"removed {len(paths)}"],
                remove_tree_fn=lambda remove_path, **_kwargs: removed.append(remove_path),
                print_fn=self.print_line,
            )
        self.assertEqual(result, 0)
        self.assertEqual(removed, [path])
        self.assertEqual(len(rollups), 2)
        self.assertEqual(json.loads(self.printed[-1])["removed"], [str(path)])

    def test_empty_and_error_paths(self):
        config = {"desktop_automation": {"retention_days": 30}}
        result = self.mod.cmd_desktop_recent(
            Namespace(target=None, action=None, limit=5, json=False),
            load_config_fn=lambda: (_ for _ in ()).throw(FileNotFoundError("missing desktop config")),
            desktop_run_manifests_fn=lambda *_args, **_kwargs: [],
            desktop_run_summary_fn=lambda _config, manifest: manifest,
            desktop_recent_lines_fn=lambda summaries, **_kwargs: [],
            short_sha_fn=lambda value: value[:12],
            print_fn=self.print_line,
        )
        self.assertEqual(result, 1)
        self.assertEqual(self.printed[-1], "Error: missing desktop config")

        result = self.mod.cmd_desktop_publish(
            Namespace(target=None, action=None, limit=5, output=None, label=None, json=False),
            load_config_fn=lambda: config,
            desktop_run_manifests_fn=lambda *_args, **_kwargs: [],
            stage_desktop_publish_report_fn=lambda *_args, **_kwargs: {},
            desktop_publish_lines_fn=lambda _report: [],
            print_fn=self.print_line,
        )
        self.assertEqual(result, 0)
        self.assertEqual(self.printed[-1], "No desktop automation runs found.")


if __name__ == "__main__":
    unittest.main()
