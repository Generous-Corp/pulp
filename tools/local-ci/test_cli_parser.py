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
            "mobile-simulator",
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
        self.assertEqual(args.video_template, "mobile-simulator")
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

    def test_desktop_video_command_accepts_terminal_reentry(self):
        parser = self.build_parser()

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

    def test_desktop_serve_accepts_background_status_and_stop_flags(self):
        parser = self.build_parser()

        args = parser.parse_args([
            "desktop",
            "serve",
            "/tmp/report",
            "--host",
            "0.0.0.0",
            "--port",
            "8768",
            "--background",
            "--label",
            "ios-proof",
            "--json",
        ])

        self.assertEqual(args.command, "desktop")
        self.assertEqual(args.desktop_command, "serve")
        self.assertEqual(args.path, "/tmp/report")
        self.assertEqual(args.host, "0.0.0.0")
        self.assertEqual(args.port, 8768)
        self.assertTrue(args.background)
        self.assertEqual(args.label, "ios-proof")
        self.assertTrue(args.json)

        args = parser.parse_args(["desktop", "serve", "--status", "--label", "ios-proof"])
        self.assertTrue(args.status)
        self.assertFalse(args.stop)
        self.assertEqual(args.label, "ios-proof")

        args = parser.parse_args(["desktop", "serve", "--stop", "--label", "ios-proof"])
        self.assertTrue(args.stop)
        self.assertFalse(args.status)

    def test_simulator_video_commands_parse(self):
        parser = self.build_parser()

        doctor = parser.parse_args(["simulator", "video-doctor", "--device", "iPhone 16", "--json"])
        self.assertEqual(doctor.command, "simulator")
        self.assertEqual(doctor.simulator_command, "video-doctor")
        self.assertEqual(doctor.device, "iPhone 16")
        self.assertTrue(doctor.json)

        video = parser.parse_args([
            "simulator",
            "video",
            "--device",
            "A-UDID",
            "--app",
            "build/ios/PulpDemo.app",
            "--bundle-id",
            "com.pulp.demo",
            "--open-url",
            "https://example.com",
            "--action-label",
            "open example.com",
            "--duration",
            "6",
            "--compose-video-proof",
            "--video-title",
            "Simulator proof",
            "--video-note",
            "URL opened",
            "--small-video",
            "--small-video-budget-mb",
            "8",
            "--video-attachment-budget-mb",
            "25",
            "--label",
            "ios-launch-proof",
            "--output",
            "/tmp/ios-proof",
            "--json",
        ])
        self.assertEqual(video.command, "simulator")
        self.assertEqual(video.simulator_command, "video")
        self.assertEqual(video.device, "A-UDID")
        self.assertEqual(video.app, "build/ios/PulpDemo.app")
        self.assertEqual(video.bundle_id, "com.pulp.demo")
        self.assertEqual(video.open_url, "https://example.com")
        self.assertEqual(video.action_label, "open example.com")
        self.assertEqual(video.duration, 6.0)
        self.assertTrue(video.compose_video_proof)
        self.assertEqual(video.video_title, "Simulator proof")
        self.assertEqual(video.video_note, ["URL opened"])
        self.assertTrue(video.small_video)
        self.assertEqual(video.small_video_budget_mb, 8.0)
        self.assertEqual(video.video_attachment_budget_mb, 25.0)
        self.assertEqual(video.label, "ios-launch-proof")
        self.assertEqual(video.output, "/tmp/ios-proof")
        self.assertTrue(video.json)

    def test_android_video_commands_parse(self):
        parser = self.build_parser()

        doctor = parser.parse_args(["android", "video-doctor", "--device", "emulator-5554", "--json"])
        self.assertEqual(doctor.command, "android")
        self.assertEqual(doctor.android_command, "video-doctor")
        self.assertEqual(doctor.device, "emulator-5554")
        self.assertTrue(doctor.json)

        video = parser.parse_args([
            "android",
            "video",
            "--device",
            "emulator-5554",
            "--apk",
            "android/app/build/outputs/apk/debug/app-debug.apk",
            "--package",
            "com.pulp.demo",
            "--activity",
            ".MainActivity",
            "--open-url",
            "pulp-demo://validate",
            "--action-label",
            "open validation deep link",
            "--duration",
            "6",
            "--compose-video-proof",
            "--video-title",
            "Android proof",
            "--video-note",
            "Deep link opened",
            "--small-video",
            "--small-video-budget-mb",
            "8",
            "--video-attachment-budget-mb",
            "25",
            "--label",
            "android-proof",
            "--output",
            "/tmp/android-proof",
            "--json",
        ])
        self.assertEqual(video.command, "android")
        self.assertEqual(video.android_command, "video")
        self.assertEqual(video.device, "emulator-5554")
        self.assertEqual(video.apk, "android/app/build/outputs/apk/debug/app-debug.apk")
        self.assertEqual(video.package, "com.pulp.demo")
        self.assertEqual(video.activity, ".MainActivity")
        self.assertEqual(video.open_url, "pulp-demo://validate")
        self.assertEqual(video.action_label, "open validation deep link")
        self.assertEqual(video.duration, 6.0)
        self.assertTrue(video.compose_video_proof)
        self.assertEqual(video.video_title, "Android proof")
        self.assertEqual(video.video_note, ["Deep link opened"])
        self.assertTrue(video.small_video)
        self.assertEqual(video.small_video_budget_mb, 8.0)
        self.assertEqual(video.video_attachment_budget_mb, 25.0)
        self.assertEqual(video.label, "android-proof")
        self.assertEqual(video.output, "/tmp/android-proof")
        self.assertTrue(video.json)

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
            "--close-issue",
            "--close-reason",
            "completed",
            "--json",
        ])

        self.assertEqual(args.desktop_command, "verdict")
        self.assertEqual(args.manifest, "/tmp/run/manifest.json")
        self.assertTrue(args.approved)
        self.assertFalse(args.needs_work)
        self.assertEqual(args.reviewer, "daniel")
        self.assertEqual(args.issue_url, "https://github.com/example/repo/issues/1")
        self.assertTrue(args.close_issue)
        self.assertEqual(args.close_reason, "completed")
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

    def test_desktop_review_issue_parses_package_and_outputs(self):
        parser = self.build_parser()

        args = parser.parse_args([
            "desktop",
            "review-issue",
            "/tmp/report/review-package.json",
            "--title",
            "Review proof",
            "--repo",
            "danielraffel/pulp",
            "--body-output",
            "/tmp/body.md",
            "--json-output",
            "/tmp/body.json",
            "--check-files",
            "--create",
            "--label",
            "video-review",
            "--assignee",
            "@me",
            "--json",
        ])

        self.assertEqual(args.desktop_command, "review-issue")
        self.assertEqual(args.path, "/tmp/report/review-package.json")
        self.assertEqual(args.title, "Review proof")
        self.assertEqual(args.repo, "danielraffel/pulp")
        self.assertEqual(args.body_output, "/tmp/body.md")
        self.assertEqual(args.json_output, "/tmp/body.json")
        self.assertTrue(args.check_files)
        self.assertTrue(args.create)
        self.assertEqual(args.label, ["video-review"])
        self.assertEqual(args.assignee, ["@me"])
        self.assertTrue(args.json)

    def test_desktop_review_status_parses_issue_and_manifest(self):
        parser = self.build_parser()

        args = parser.parse_args([
            "desktop",
            "review-status",
            "https://github.com/danielraffel/pulp/issues/123",
            "--repo",
            "danielraffel/pulp",
            "--manifest",
            "/tmp/run/manifest.json",
            "--close-issue",
            "--json",
        ])

        self.assertEqual(args.desktop_command, "review-status")
        self.assertEqual(args.issue_url, "https://github.com/danielraffel/pulp/issues/123")
        self.assertEqual(args.repo, "danielraffel/pulp")
        self.assertEqual(args.manifest, "/tmp/run/manifest.json")
        self.assertTrue(args.close_issue)
        self.assertTrue(args.json)

    def test_desktop_video_matrix_parses_filters_and_formats(self):
        parser = self.build_parser()

        args = parser.parse_args([
            "desktop",
            "video-matrix",
            "--target",
            "mac",
            "--scenario",
            "component-zoom",
            "--markdown",
            "--check",
        ])

        self.assertEqual(args.desktop_command, "video-matrix")
        self.assertEqual(args.target, "mac")
        self.assertEqual(args.scenario, "component-zoom")
        self.assertTrue(args.markdown)
        self.assertTrue(args.check)
        self.assertFalse(args.json)

        args = parser.parse_args([
            "desktop",
            "video-matrix",
            "--target",
            "windows",
            "--scenario",
            "windows-session-agent-desktop",
            "--json",
        ])

        self.assertEqual(args.target, "windows")
        self.assertEqual(args.scenario, "windows-session-agent-desktop")
        self.assertTrue(args.json)
        self.assertFalse(args.check)

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
            "--recipe",
            "reaper-plugin-editor",
            "--plugin",
            "PulpSynth",
            "--plugin-format",
            "clap",
        ])

        self.assertEqual(args.command, "desktop")
        self.assertEqual(args.desktop_command, "video-doctor")
        self.assertEqual(args.target, "mac")
        self.assertTrue(args.skip_remotion_smoke)
        self.assertTrue(args.json)
        self.assertTrue(args.run_in_terminal)
        self.assertEqual(args.video_audio, "system")
        self.assertEqual(args.video_audio_device, "BlackHole 2ch")
        self.assertEqual(args.recipe, "reaper-plugin-editor")
        self.assertEqual(args.plugin, "PulpSynth")
        self.assertEqual(args.plugin_format, "clap")

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

    def test_desktop_compose_video_accepts_mobile_template(self):
        parser = self.build_parser()

        args = parser.parse_args([
            "desktop",
            "compose-video",
            "/tmp/run/manifest.json",
            "--template",
            "mobile-simulator",
        ])

        self.assertEqual(args.desktop_command, "compose-video")
        self.assertEqual(args.template, "mobile-simulator")

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
