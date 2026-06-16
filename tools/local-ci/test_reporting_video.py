#!/usr/bin/env python3
"""No-network tests for reporting_video proof/report helpers."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("reporting_video.py")


def load_module():
    spec = importlib.util.spec_from_file_location("reporting_video_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class ReportingVideoTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_github_attachment_policy_lists_limits_and_extensions(self) -> None:
        policy = self.mod._github_video_attachment_policy()
        self.assertIn(".mp4", policy["supported_video_extensions"])
        self.assertEqual(policy["pro_video_limit_bytes"], 100_000_000)
        self.assertEqual(policy["free_video_limit_bytes"], 10_000_000)
        self.assertTrue(policy["source"].startswith("https://docs.github.com/"))

    def test_proof_notes_dedupe_across_manifest_and_composition(self) -> None:
        manifest = {
            "video_proof_notes": ["  toggle flips  ", "toggle flips", 7],
            "video_proof_composition": {"notes": ["audio rises", "toggle flips"]},
        }
        self.assertEqual(
            self.mod._proof_notes_from_manifest(manifest),
            ["toggle flips", "audio rises"],
        )
        self.assertEqual(self.mod._proof_notes_from_manifest({}), [])

    def test_copy_optional_file_handles_missing_and_real(self) -> None:
        self.assertFalse(self.mod._copy_optional_file(None, self.root / "out.bin"))
        self.assertFalse(self.mod._copy_optional_file("", self.root / "out.bin"))
        self.assertFalse(self.mod._copy_optional_file(str(self.root / "nope"), self.root / "out.bin"))
        src = self.root / "src.txt"
        src.write_text("hi")
        dest = self.root / "nested" / "dest.txt"
        self.assertTrue(self.mod._copy_optional_file(str(src), dest))
        self.assertEqual(dest.read_text(), "hi")

    def test_artifact_metadata_reads_json_and_tolerates_errors(self) -> None:
        self.assertEqual(self.mod._artifact_metadata(self.root, None), {})
        self.assertEqual(self.mod._artifact_metadata(self.root, "missing.json"), {})
        good = self.root / "good.json"
        good.write_text(json.dumps({"a": 1}))
        self.assertEqual(self.mod._artifact_metadata(self.root, "good.json"), {"a": 1})
        bad = self.root / "bad.json"
        bad.write_text("not json")
        self.assertEqual(self.mod._artifact_metadata(self.root, "bad.json"), {})

    def test_format_bytes_picks_mb_kb_or_unknown(self) -> None:
        self.assertEqual(self.mod._format_bytes(None), "unknown")
        self.assertEqual(self.mod._format_bytes(2_500_000), "2.5 MB")
        self.assertEqual(self.mod._format_bytes(2000), "2 KB")
        self.assertEqual(self.mod._format_bytes(10), "1 KB")

    def test_proof_focus_summary_and_label(self) -> None:
        self.assertEqual(self.mod._proof_focus_summary({}), {})
        comp = {
            "focus": {
                "selector": {"click_view_id": "bypass"},
                "content_point": {"x": 1, "y": 2},
                "normalized_center": {"x": 0.2, "y": 0.5},
            }
        }
        summary = self.mod._proof_focus_summary(comp)
        self.assertEqual(summary["label"], "bypass")
        self.assertEqual(summary["content_point"], {"x": 1, "y": 2})
        self.assertEqual(self.mod._proof_focus_label(comp), "bypass")
        self.assertIsNone(self.mod._proof_focus_label({}))

    def test_proof_action_marker_summary(self) -> None:
        self.assertEqual(self.mod._proof_action_marker_summary({}), {})
        marker = {
            "action_marker": {
                "kind": "tap",
                "label": "Bypass",
                "content_point": {"x": 3, "y": 4},
                "normalized_point": {"x": 0.2, "y": 0.5},
            }
        }
        summary = self.mod._proof_action_marker_summary(marker)
        self.assertEqual(summary["kind"], "tap")
        self.assertEqual(summary["normalized_point"], {"x": 0.2, "y": 0.5})

    def test_proof_context_items_skips_none(self) -> None:
        self.assertEqual(self.mod._proof_context_items({}), [])
        items = self.mod._proof_context_items({"context": {"recipe": "ui-toggle", "skip": None, "n": 3}})
        self.assertIn(("recipe", "ui-toggle"), items)
        self.assertIn(("n", "3"), items)
        self.assertNotIn("skip", [k for k, _ in items])

    def test_proof_storyboard_from_metadata_and_lines(self) -> None:
        self.assertEqual(self.mod._proof_storyboard_from_metadata({}), {})
        metadata = {
            "review_storyboard": {
                "title": "Toggle proof",
                "steps": [
                    {"label": "Launch", "detail": "open app"},
                    {"label": "Tap", "detail": "tap bypass"},
                    "ignored",
                ],
                "notes": ["clean room"],
            }
        }
        storyboard = self.mod._proof_storyboard_from_metadata(metadata)
        self.assertEqual(storyboard["title"], "Toggle proof")
        self.assertEqual(len(storyboard["steps"]), 2)
        lines = self.mod._proof_storyboard_lines(storyboard)
        self.assertEqual(lines[0], "Launch: open app")
        self.assertEqual(self.mod._proof_storyboard_lines({}), [])

    def test_manifest_command_text_variants(self) -> None:
        self.assertEqual(self.mod._manifest_command_text({"command": ["a", "b c"]}), "a 'b c'")
        self.assertEqual(self.mod._manifest_command_text({"command": " run "}), "run")
        self.assertEqual(self.mod._manifest_command_text({"bundle_id": "com.x.y"}), "open -b com.x.y")
        self.assertEqual(self.mod._manifest_command_text({"app_path": "/A.app"}), "open /A.app")
        self.assertIsNone(self.mod._manifest_command_text({}))

    def test_manifest_source_context_and_summary(self) -> None:
        self.assertEqual(self.mod._manifest_source_context({}), {})
        manifest = {"source": {"mode": "exact-sha", "sha": "abc", "branch": "feat", "extra": None}}
        ctx = self.mod._manifest_source_context(manifest)
        self.assertEqual(ctx, {"mode": "exact-sha", "sha": "abc", "branch": "feat"})
        self.assertEqual(
            self.mod._source_summary(manifest["source"]),
            "mode=exact-sha, branch=feat, sha=abc",
        )
        self.assertIsNone(self.mod._source_summary({}))

    def test_serve_commands_and_label(self) -> None:
        publish_dir = self.root / "report dir"
        publish_dir.mkdir()
        label = self.mod._desktop_report_serve_label({"label": "My Proof"}, publish_dir)
        self.assertEqual(label, "my-proof")
        commands = self.mod._desktop_report_serve_commands({"label": "My Proof"}, publish_dir)
        self.assertEqual(commands["serve_label"], "my-proof")
        self.assertIn("desktop serve", commands["serve_command"])
        self.assertIn("'" + str(publish_dir) + "'", commands["serve_command"])
        fallback = self.mod._desktop_report_serve_label({}, publish_dir)
        self.assertTrue(fallback)


if __name__ == "__main__":
    unittest.main()
