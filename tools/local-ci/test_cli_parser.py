#!/usr/bin/env python3
from __future__ import annotations

import importlib.util
from pathlib import Path
import unittest


MODULE_PATH = Path(__file__).resolve().with_name("cli_parser.py")


def load_cli_parser_module():
    spec = importlib.util.spec_from_file_location("cli_parser_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class CliParserTests(unittest.TestCase):
    def build_parser(self, *, keep_completed_jobs: int = 25):
        module = load_cli_parser_module()
        return module.build_local_ci_parser(
            priority_values={"low", "normal", "high"},
            keep_completed_jobs=keep_completed_jobs,
            epilog="test epilog",
        )

    def test_run_submission_args(self):
        parser = self.build_parser()

        args = parser.parse_args([
            "run",
            "feature/local-ci",
            "--priority",
            "high",
            "--targets",
            "mac,ubuntu",
            "--sha",
            "a" * 40,
            "--smoke",
            "--allow-root-mismatch",
            "--allow-unreachable-targets",
        ])

        self.assertEqual(args.command, "run")
        self.assertEqual(args.branch, "feature/local-ci")
        self.assertEqual(args.priority, "high")
        self.assertEqual(args.targets, "mac,ubuntu")
        self.assertEqual(args.sha, "a" * 40)
        self.assertTrue(args.smoke)
        self.assertTrue(args.allow_root_mismatch)
        self.assertTrue(args.allow_unreachable_targets)

    def test_cloud_namespace_commands(self):
        parser = self.build_parser()

        args = parser.parse_args(["cloud", "namespace", "doctor"])

        self.assertEqual(args.command, "cloud")
        self.assertEqual(args.cloud_command, "namespace")
        self.assertEqual(args.cloud_namespace_command, "doctor")

    def test_desktop_source_args_are_shared(self):
        parser = self.build_parser()

        args = parser.parse_args([
            "desktop",
            "click",
            "mac",
            "--source-mode",
            "exact-sha",
            "--sha",
            "b" * 40,
            "--prepare-command",
            "cmake --build build",
            "--click",
            "12,34",
        ])

        self.assertEqual(args.command, "desktop")
        self.assertEqual(args.desktop_command, "click")
        self.assertEqual(args.target, "mac")
        self.assertEqual(args.source_mode, "exact-sha")
        self.assertEqual(args.sha, "b" * 40)
        self.assertEqual(args.prepare_command, "cmake --build build")
        self.assertEqual(args.prepare_timeout, 900.0)
        self.assertEqual(args.click, "12,34")

    def test_desktop_video_command_defaults_to_click_proof(self):
        parser = self.build_parser()

        args = parser.parse_args([
            "desktop",
            "video",
            "mac",
            "--command",
            "./build/pulp",
            "--click-view-id",
            "bypass-toggle",
            "--duration",
            "6",
            "--video-fps",
            "24",
            "--recipe",
            "component-zoom",
            "--component-id",
            "bypass-toggle",
            "--video-template",
            "validation-proof",
            "--video-title",
            "Bypass proof",
            "--video-note",
            "Click bypass",
            "--video-note",
            "Meter changed",
            "--video-audio",
            "system",
            "--video-audio-device",
            "BlackHole 2ch",
            "--capture-bundle-id",
            "com.cockos.reaper",
            "--label",
            "standalone-bypass-toggle",
        ])

        self.assertEqual(args.command, "desktop")
        self.assertEqual(args.desktop_command, "video")
        self.assertEqual(args.target, "mac")
        self.assertEqual(args.action, "click")
        self.assertEqual(args.recipe, "component-zoom")
        self.assertEqual(args.component_id, "bypass-toggle")
        self.assertEqual(args.launch_command, "./build/pulp")
        self.assertEqual(args.click_view_id, "bypass-toggle")
        self.assertFalse(args.record_video)
        self.assertFalse(args.compose_video_proof)
        self.assertEqual(args.video_template, "validation-proof")
        self.assertEqual(args.video_title, "Bypass proof")
        self.assertEqual(args.video_note, ["Click bypass", "Meter changed"])
        self.assertEqual(args.video_duration, 6.0)
        self.assertEqual(args.video_fps, 24.0)
        self.assertEqual(args.video_audio, "system")
        self.assertEqual(args.video_audio_device, "BlackHole 2ch")
        self.assertEqual(args.capture_bundle_id, "com.cockos.reaper")
        self.assertEqual(args.video_attachment_budget_mb, 100.0)
        self.assertEqual(args.label, "standalone-bypass-toggle")

    def test_desktop_video_command_accepts_audio_inspector_recipe(self):
        parser = self.build_parser()

        args = parser.parse_args([
            "desktop",
            "video",
            "mac",
            "--recipe",
            "audio-inspector-demo",
            "--command",
            "./build-video-nogpu/examples/audio-inspector-demo/pulp-audio-inspector-demo",
        ])

        self.assertEqual(args.recipe, "audio-inspector-demo")
        self.assertEqual(
            args.launch_command,
            "./build-video-nogpu/examples/audio-inspector-demo/pulp-audio-inspector-demo",
        )
        self.assertFalse(args.run_in_terminal)

        args = parser.parse_args([
            "desktop",
            "video",
            "mac",
            "--bundle-id",
            "com.apple.TextEdit",
            "--action",
            "smoke",
            "--run-in-terminal",
        ])
        self.assertTrue(args.run_in_terminal)

    def test_desktop_verdict_command_requires_explicit_status(self):
        parser = self.build_parser()

        args = parser.parse_args([
            "desktop",
            "verdict",
            "/tmp/run/manifest.json",
            "--approved",
            "--reviewer",
            "daniel",
            "--issue-url",
            "https://github.com/example/repo/issues/1",
            "--json",
        ])

        self.assertEqual(args.desktop_command, "verdict")
        self.assertEqual(args.manifest, "/tmp/run/manifest.json")
        self.assertTrue(args.approved)
        self.assertFalse(args.needs_work)
        self.assertEqual(args.reviewer, "daniel")
        self.assertEqual(args.issue_url, "https://github.com/example/repo/issues/1")
        self.assertTrue(args.json)

    def test_desktop_publish_parses_explicit_manifests(self):
        parser = self.build_parser()

        args = parser.parse_args([
            "desktop",
            "publish",
            "--manifest",
            "/tmp/run-a/manifest.json",
            "--manifest",
            "/tmp/run-b/manifest.json",
            "--label",
            "video-review",
        ])

        self.assertEqual(args.desktop_command, "publish")
        self.assertIsNone(args.target)
        self.assertEqual(args.manifest, ["/tmp/run-a/manifest.json", "/tmp/run-b/manifest.json"])
        self.assertEqual(args.label, "video-review")

    def test_desktop_video_doctor_defaults_to_mac(self):
        parser = self.build_parser()

        args = parser.parse_args([
            "desktop",
            "video-doctor",
            "--skip-remotion-smoke",
            "--json",
            "--run-in-terminal",
            "--video-audio",
            "system",
            "--video-audio-device",
            "BlackHole 2ch",
        ])

        self.assertEqual(args.command, "desktop")
        self.assertEqual(args.desktop_command, "video-doctor")
        self.assertEqual(args.target, "mac")
        self.assertTrue(args.skip_remotion_smoke)
        self.assertTrue(args.json)
        self.assertTrue(args.run_in_terminal)
        self.assertEqual(args.video_audio, "system")
        self.assertEqual(args.video_audio_device, "BlackHole 2ch")

    def test_desktop_video_setup_command(self):
        parser = self.build_parser()

        args = parser.parse_args([
            "desktop",
            "video-setup",
            "mac",
            "--machine",
            "blackbook",
            "--check",
            "--run-in-terminal",
            "--skip-remotion-smoke",
            "--video-audio",
            "system",
            "--video-audio-device",
            "BlackHole 2ch",
            "--json",
        ])

        self.assertEqual(args.command, "desktop")
        self.assertEqual(args.desktop_command, "video-setup")
        self.assertEqual(args.target, "mac")
        self.assertEqual(args.machine, "blackbook")
        self.assertTrue(args.check)
        self.assertTrue(args.run_in_terminal)
        self.assertTrue(args.skip_remotion_smoke)
        self.assertEqual(args.video_audio, "system")
        self.assertEqual(args.video_audio_device, "BlackHole 2ch")
        self.assertTrue(args.json)

    def test_desktop_compose_video_command_parses_outputs(self):
        parser = self.build_parser()

        args = parser.parse_args([
            "desktop",
            "compose-video",
            "/tmp/run/manifest.json",
            "--output",
            "/tmp/run/video/custom.mp4",
            "--issue-output",
            "/tmp/run/video/custom.issue.mp4",
            "--small-video",
            "--small-output",
            "/tmp/run/video/custom.small.mp4",
            "--small-metadata",
            "/tmp/run/video/custom-small.json",
            "--small-video-budget-mb",
            "10",
            "--template",
            "design-parity",
            "--source-image",
            "/tmp/source.png",
            "--source-label",
            "Figma reference",
            "--title",
            "Design parity",
            "--note",
            "Reference matches implementation",
            "--video-attachment-budget-mb",
            "40",
            "--json",
        ])

        self.assertEqual(args.desktop_command, "compose-video")
        self.assertEqual(args.manifest, "/tmp/run/manifest.json")
        self.assertEqual(args.output, "/tmp/run/video/custom.mp4")
        self.assertEqual(args.issue_output, "/tmp/run/video/custom.issue.mp4")
        self.assertTrue(args.small_video)
        self.assertEqual(args.small_output, "/tmp/run/video/custom.small.mp4")
        self.assertEqual(args.small_metadata, "/tmp/run/video/custom-small.json")
        self.assertEqual(args.small_video_budget_mb, 10.0)
        self.assertEqual(args.template, "design-parity")
        self.assertEqual(args.source_image, "/tmp/source.png")
        self.assertEqual(args.source_label, "Figma reference")
        self.assertEqual(args.title, "Design parity")
        self.assertEqual(args.note, ["Reference matches implementation"])
        self.assertEqual(args.video_attachment_budget_mb, 40.0)
        self.assertTrue(args.json)

    def test_cleanup_defaults_use_injected_retention(self):
        parser = self.build_parser(keep_completed_jobs=7)

        args = parser.parse_args(["cleanup"])

        self.assertEqual(args.command, "cleanup")
        self.assertEqual(args.keep_results, 7)
        self.assertEqual(args.keep_logs, 7)
        self.assertEqual(args.keep_bundles, 0)
        self.assertFalse(args.include_prepared)
        self.assertFalse(args.dry_run)
        self.assertFalse(args.apply)


if __name__ == "__main__":
    unittest.main()
