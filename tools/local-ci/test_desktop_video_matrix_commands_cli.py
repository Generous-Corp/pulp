#!/usr/bin/env python3
"""Tests for the desktop video-matrix command."""

from argparse import Namespace
import json
import os
from pathlib import Path
import tempfile
import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("desktop_video_matrix_commands_cli.py")


class DesktopVideoMatrixCommandsTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()
        self.printed: list[str] = []

    def print_line(self, line: str):
        self.printed.append(line)

    def desktop_config(self):
        return {
            "desktop_automation": {
                "artifact_root": "runs",
                "publish_mode": "local",
                "publish_branch": "desktop-artifacts",
                "retention_days": 7,
                "targets": {"mac": {"enabled": True, "adapter": "macos-local", "bootstrap": "launchagent", "target_type": "local", "capability_tier": "full", "optional": {"webview_driver": True}}},
            }
        }

    def test_desktop_video_matrix_outputs_text_json_and_markdown(self):
        result = self.mod.cmd_desktop_video_matrix(
            Namespace(target="mac", scenario="component-zoom", json=False, markdown=False),
            print_fn=self.print_line,
        )
        self.assertEqual(result, 0)
        text = "\n".join(self.printed)
        self.assertIn("Desktop validation video proof demo matrix:", text)
        self.assertIn("status: all (declared)", text)
        self.assertIn("component-zoom [ready]", text)
        self.assertIn("--recipe component-zoom", text)
        self.assertIn("build-desktop-automation/examples/ui-preview/pulp-ui-preview", text)
        self.assertIn("prepare: cmake -S . -B build-desktop-automation -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=OFF", text)
        self.assertIn("--source-mode exact-sha", text)
        self.assertIn("--prepare-command 'cmake -S . -B build-desktop-automation -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=OFF", text)
        self.assertIn("--pulp-app-automation", text)
        self.assertIn("--component-id bypass-toggle", text)
        self.assertIn("--click-view-id bypass-toggle", text)
        self.assertNotIn("compressor-threshold", text)
        self.assertIn("publish:", text)
        self.assertIn("review issue:", text)
        self.assertIn("review create:", text)
        self.assertIn("review status:", text)
        self.assertIn("review watch:", text)
        self.assertIn("desktop review-status <issue-url>", text)
        self.assertIn("desktop review-issue /path/to/published-reports/component-zoom --repo owner/repo --check-files", text)
        self.assertIn("--check-files --create --label video-review --manifest-map-output /tmp/component-zoom-video-review-manifest-map.json", text)
        self.assertIn("--manifest-map-output /tmp/component-zoom-video-review-manifest-map.json", text)
        self.assertIn("desktop review-watch --repo owner/repo --label video-review", text)
        self.assertIn("--background --label component-zoom-review --json", text)
        self.assertIn("cleanup published:", text)
        self.assertIn("desktop cleanup --published --older-than-days 14 --keep-last 3 --json", text)
        self.assertNotIn("ios-simulator", text)

        self.printed.clear()
        result = self.mod.cmd_desktop_video_matrix(
            Namespace(target=None, scenario=None, json=True, markdown=False),
            print_fn=self.print_line,
        )
        self.assertEqual(result, 0)
        payload = json.loads(self.printed[0])
        self.assertEqual(payload["kind"], "desktop-video-proof-demo-matrix")
        self.assertEqual(payload["status"], "all")
        self.assertEqual(payload["status_basis"], "declared")
        self.assertEqual(payload["scenario_count"], 10)
        self.assertIn("reaper-plugin-editor", {item["id"] for item in payload["scenarios"]})
        reaper = next(item for item in payload["scenarios"] if item["id"] == "reaper-plugin-editor")
        self.assertIn("desktop publish --manifest /path/to/run/manifest.json", reaper["publish_command"])
        self.assertIn("desktop review-issue /path/to/published-reports/reaper-plugin-editor", reaper["review_issue_command"])
        self.assertNotIn("--create", reaper["review_issue_command"])
        self.assertNotIn("--manifest-map-output", reaper["review_issue_command"])
        self.assertIn("--create --label video-review", reaper["review_create_command"])
        self.assertIn("--manifest-map-output /tmp/reaper-plugin-editor-video-review-manifest-map.json", reaper["review_create_command"])
        self.assertIn("desktop review-status <issue-url>", reaper["review_status_command"])
        self.assertIn("--manifest /path/to/run/manifest.json --close-issue", reaper["review_status_command"])
        self.assertEqual(reaper["review_manifest_map"], "/tmp/reaper-plugin-editor-video-review-manifest-map.json")
        self.assertIn("desktop review-watch", reaper["review_watch_command"])
        self.assertIn("--manifest-map /tmp/reaper-plugin-editor-video-review-manifest-map.json", reaper["review_watch_command"])
        self.assertIn("--close-issue", reaper["review_watch_command"])
        self.assertIn("--auto-port", reaper["serve_background_command"])
        self.assertIn("--background --label reaper-plugin-editor-review --json", reaper["serve_background_command"])
        self.assertIn("PulpSynth_CLAP", reaper["prepare_command"])
        self.assertIn("Plug-Ins/CLAP/PulpSynth.clap", reaper["command"])
        self.assertEqual(reaper["review_workflow"][0]["step"], "prepare")
        self.assertEqual(reaper["review_workflow"][1]["step"], "doctor")
        self.assertIn("review_create_command", reaper)
        self.assertIn("desktop cleanup --published", reaper["published_cleanup_command"])
        workflow_steps = [step["step"] for step in reaper["review_workflow"]]
        self.assertIn("draft_issue", workflow_steps)
        self.assertIn("create_issue_with_map", workflow_steps)
        self.assertLess(workflow_steps.index("draft_issue"), workflow_steps.index("create_issue_with_map"))
        self.assertEqual(reaper["review_workflow"][-4]["step"], "check_review")
        self.assertEqual(reaper["review_workflow"][-3]["step"], "watch_reviews")
        self.assertEqual(reaper["review_workflow"][-2]["step"], "stop_server")
        self.assertEqual(reaper["review_workflow"][-1]["step"], "cleanup_published_reports")
        standalone = next(item for item in payload["scenarios"] if item["id"] == "standalone-interaction")
        self.assertEqual(
            standalone["prepare_command"],
            'cmake -S . -B build-desktop-automation -DCMAKE_BUILD_TYPE=Release -DPULP_BUILD_TESTS=OFF -DSKIA_DIR="$(pwd)/external/skia-build" && '
            "cmake --build build-desktop-automation --target pulp-ui-preview -j$(sysctl -n hw.ncpu)",
        )
        self.assertIn("--prepare-command", standalone["command"])
        self.assertIn('-DSKIA_DIR="$(pwd)/external/skia-build"', standalone["command"])
        self.assertIn("--source-mode exact-sha", standalone["command"])
        self.assertIn("--pulp-app-automation", standalone["command"])
        self.assertIn("./build-desktop-automation/examples/ui-preview/pulp-ui-preview", standalone["command"])
        self.assertIn("--duration 6 --video-fps 8", standalone["command"])
        self.assertIn("--video-recorder frame-sequence", standalone["command"])
        audio_demo = next(item for item in payload["scenarios"] if item["id"] == "audio-inspector-demo")
        self.assertEqual(audio_demo["status"], "ready")
        self.assertEqual(audio_demo["template"], "inspector-workflow")
        self.assertIn("-DPULP_ENABLE_GPU=OFF", audio_demo["prepare_command"])
        self.assertIn("--recipe audio-inspector-demo", audio_demo["command"])
        self.assertIn("./build-video-nogpu/examples/audio-inspector-demo/pulp-audio-inspector-demo", audio_demo["command"])
        self.assertIn("--duration 4 --video-fps 8", audio_demo["command"])
        self.assertIn("--compose-video-proof", audio_demo["command"])
        self.assertNotIn("--capture-ui-snapshot", audio_demo["command"])
        self.assertNotIn("--pulp-app-automation", audio_demo["command"])
        inspector = next(item for item in payload["scenarios"] if item["id"] == "inspector-workflow")
        self.assertIn("-DPULP_ENABLE_GPU=OFF", inspector["prepare_command"])
        self.assertIn("./build-video-nogpu/examples/audio-inspector-demo/pulp-audio-inspector-demo", inspector["command"])
        self.assertNotIn("--capture-ui-snapshot", inspector["command"])
        component = next(item for item in payload["scenarios"] if item["id"] == "component-zoom")
        self.assertIn('-DSKIA_DIR="$(pwd)/external/skia-build"', component["prepare_command"])
        self.assertIn('-DSKIA_DIR="$(pwd)/external/skia-build"', component["command"])
        self.assertIn("--duration 6 --video-fps 8", component["command"])
        self.assertIn("--video-recorder frame-sequence", component["command"])
        linux = next(item for item in payload["scenarios"] if item["id"] == "linux-xvfb-desktop")
        self.assertEqual(linux["platform"], "ubuntu")
        self.assertEqual(linux["status"], "planned")
        self.assertIn("video-doctor ubuntu", linux["doctor"])
        self.assertIn("x11grab", " ".join(linux["watch_for"]))
        windows = next(item for item in payload["scenarios"] if item["id"] == "windows-session-agent-desktop")
        self.assertEqual(windows["platform"], "windows")
        self.assertEqual(windows["status"], "planned")
        self.assertIn("video-doctor windows", windows["doctor"])
        self.assertIn("ddagrab/gdigrab", " ".join(windows["watch_for"]))

    def test_desktop_video_matrix_check_reports_local_blockers(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            repo_root = Path(tmpdir)
            payload = self.mod.desktop_video_matrix_payload(
                scenario="component-zoom",
                check=True,
                repo_root=repo_root,
                which_fn=lambda name: "/usr/bin/cmake" if name == "cmake" else None,
            )
            row = payload["scenarios"][0]
            self.assertTrue(payload["checked"])
            self.assertEqual(row["status"], "blocked")
            self.assertEqual(row["declared_status"], "ready")
            self.assertEqual(row["local_readiness"]["status"], "blocked")
            checks = {check["name"]: check for check in row["local_readiness"]["checks"]}
            self.assertTrue(checks["cmake"]["ok"])
            self.assertFalse(checks["skia-build.libskia"]["ok"])
            self.assertIn("missing required Skia binary", checks["skia-build.libskia"]["detail"])
            self.assertIn("fetch_skia_for_release.py darwin-arm64", checks["skia-build.libskia"]["remediation"])
            self.assertNotIn("remediation", checks["cmake"])

            skia_lib = repo_root / "external" / "skia-build" / "build" / "mac-gpu" / "lib" / "Release" / "libskia.a"
            skia_lib.parent.mkdir(parents=True)
            skia_lib.write_bytes(b"skia")
            payload = self.mod.desktop_video_matrix_payload(
                scenario="component-zoom",
                check=True,
                repo_root=repo_root,
                which_fn=lambda name: "/usr/bin/cmake" if name == "cmake" else None,
            )
            self.assertEqual(payload["scenarios"][0]["status"], "ready")
            self.assertEqual(payload["scenarios"][0]["declared_status"], "ready")
            self.assertEqual(payload["scenarios"][0]["local_readiness"]["status"], "ready")

            payload = self.mod.desktop_video_matrix_payload(
                scenario="audio-inspector-demo",
                check=True,
                repo_root=repo_root,
                which_fn=lambda name: "/usr/bin/cmake" if name == "cmake" else None,
            )
            audio_demo = payload["scenarios"][0]
            checks = {check["name"]: check for check in audio_demo["local_readiness"]["checks"]}
            self.assertEqual(audio_demo["status"], "blocked")
            self.assertEqual(audio_demo["local_readiness"]["status"], "blocked")
            self.assertTrue(checks["cmake"]["ok"])
            self.assertFalse(checks["audio-inspector-demo-source"]["ok"])
            self.assertIn("examples/audio-inspector-demo", checks["audio-inspector-demo-source"]["remediation"])
            self.assertNotIn("skia-build.libskia", checks)

            (repo_root / "examples" / "audio-inspector-demo").mkdir(parents=True)
            payload = self.mod.desktop_video_matrix_payload(
                scenario="audio-inspector-demo",
                check=True,
                repo_root=repo_root,
                which_fn=lambda name: "/usr/bin/cmake" if name == "cmake" else None,
            )
            self.assertEqual(payload["scenarios"][0]["status"], "ready")
            self.assertEqual(payload["scenarios"][0]["local_readiness"]["status"], "ready")

    def test_desktop_video_matrix_check_accepts_skia_dir_override(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            repo_root = Path(tmpdir) / "repo"
            skia_root = Path(tmpdir) / "skia"
            skia_lib = skia_root / "mac-gpu" / "lib" / "Release" / "libskia.a"
            skia_lib.parent.mkdir(parents=True)
            skia_lib.write_bytes(b"skia")
            old_skia_dir = os.environ.get("SKIA_DIR")
            os.environ["SKIA_DIR"] = str(skia_root)
            try:
                payload = self.mod.desktop_video_matrix_payload(
                    scenario="component-zoom",
                    check=True,
                    repo_root=repo_root,
                    which_fn=lambda name: "/usr/bin/cmake" if name == "cmake" else None,
                )
            finally:
                if old_skia_dir is None:
                    os.environ.pop("SKIA_DIR", None)
                else:
                    os.environ["SKIA_DIR"] = old_skia_dir
            row = payload["scenarios"][0]
            self.assertEqual(row["status"], "ready")
            checks = {check["name"]: check for check in row["local_readiness"]["checks"]}
            self.assertTrue(checks["skia-build.libskia"]["ok"])
            self.assertEqual(checks["skia-build.libskia"]["detail"], str(skia_lib.resolve()))

    def test_desktop_video_matrix_filters_by_status(self):
        static_payload = self.mod.desktop_video_matrix_payload(
            target="mac",
            status="partial",
        )
        self.assertFalse(static_payload["checked"])
        self.assertEqual(static_payload["status"], "partial")
        self.assertEqual(static_payload["status_basis"], "declared")
        self.assertEqual(static_payload["scenarios"], [])

        ready_payload = self.mod.desktop_video_matrix_payload(
            target="mac",
            status="ready",
        )
        self.assertIn("reaper-plugin-editor", {item["id"] for item in ready_payload["scenarios"]})

        with tempfile.TemporaryDirectory() as tmpdir:
            repo_root = Path(tmpdir)
            (repo_root / "examples" / "audio-inspector-demo").mkdir(parents=True)
            checked_payload = self.mod.desktop_video_matrix_payload(
                target="mac",
                status="ready",
                check=True,
                repo_root=repo_root,
                which_fn=lambda name: "/usr/bin/cmake" if name == "cmake" else None,
            )
            self.assertTrue(checked_payload["checked"])
            self.assertEqual(checked_payload["status"], "ready")
            self.assertEqual(checked_payload["status_basis"], "local_readiness")
            ids = {item["id"] for item in checked_payload["scenarios"]}
            self.assertIn("audio-inspector-demo", ids)
            self.assertIn("inspector-workflow", ids)
            self.assertNotIn("design-parity", ids)
            self.assertNotIn("component-zoom", ids)
            self.assertNotIn("standalone-interaction", ids)

            blocked_payload = self.mod.desktop_video_matrix_payload(
                target="mac",
                status="blocked",
                check=True,
                repo_root=repo_root,
                which_fn=lambda name: "/usr/bin/cmake" if name == "cmake" else None,
            )
            blocked_ids = {item["id"] for item in blocked_payload["scenarios"]}
            self.assertIn("component-zoom", blocked_ids)
            self.assertIn("standalone-interaction", blocked_ids)
            self.assertIn("design-parity", blocked_ids)
            self.assertNotIn("audio-inspector-demo", blocked_ids)
            design_parity = next(item for item in blocked_payload["scenarios"] if item["id"] == "design-parity")
            design_checks = {check["name"]: check for check in design_parity["local_readiness"]["checks"]}
            self.assertFalse(design_checks["design-parity.run-manifest"]["ok"])
            self.assertIn("--design-parity-manifest", design_checks["design-parity.run-manifest"]["remediation"])
            self.assertFalse(design_checks["design-parity.native-image"]["ok"])
            self.assertIn("--design-parity-native-image", design_checks["design-parity.native-image"]["remediation"])
            self.assertFalse(design_checks["design-parity.source-image"]["ok"])
            self.assertIn("--design-parity-source-image", design_checks["design-parity.source-image"]["remediation"])
            for row in blocked_payload["scenarios"]:
                self.assertEqual(row["status"], "blocked")
                self.assertEqual(row["local_readiness"]["status"], "blocked")

            (repo_root / "planning" / "screenshots").mkdir(parents=True)
            (repo_root / "planning" / "screenshots" / "reference.png").write_bytes(b"png")
            checked_payload = self.mod.desktop_video_matrix_payload(
                target="mac",
                status="ready",
                check=True,
                repo_root=repo_root,
                which_fn=lambda name: "/usr/bin/cmake" if name == "cmake" else None,
            )
            self.assertNotIn("design-parity", {item["id"] for item in checked_payload["scenarios"]})

            manifest_path = repo_root / "runs" / "design" / "manifest.json"
            manifest_path.parent.mkdir(parents=True)
            manifest_path.write_text(json.dumps({"label": "design-parity-base-run"}) + "\n")
            checked_payload = self.mod.desktop_video_matrix_payload(
                target="mac",
                status="ready",
                check=True,
                design_parity_manifest=manifest_path,
                repo_root=repo_root,
                which_fn=lambda name: "/usr/bin/cmake" if name == "cmake" else None,
            )
            design_ready = next(item for item in checked_payload["scenarios"] if item["id"] == "design-parity")
            self.assertIn(str(manifest_path.resolve()), design_ready["command"])
            self.assertIn(str((repo_root / "planning" / "screenshots" / "reference.png").resolve()), design_ready["command"])
            self.assertIn("desktop compose-video", design_ready["command"])
            self.assertEqual(design_ready["local_readiness"]["status"], "ready")
            design_checks = {check["name"]: check for check in design_ready["local_readiness"]["checks"]}
            self.assertTrue(design_checks["design-parity.run-manifest"]["ok"])
            self.assertFalse(design_checks["design-parity.native-image"]["ok"])
            self.assertFalse(design_checks["design-parity.native-image"]["required"])

            native_path = repo_root / "renders" / "native.png"
            native_path.parent.mkdir()
            native_path.write_bytes(b"png")
            checked_payload = self.mod.desktop_video_matrix_payload(
                target="mac",
                status="ready",
                check=True,
                design_parity_source_image=repo_root / "planning" / "screenshots" / "reference.png",
                design_parity_native_image=native_path,
                repo_root=repo_root,
                which_fn=lambda name: "/usr/bin/cmake" if name == "cmake" else None,
            )
            design_ready = next(item for item in checked_payload["scenarios"] if item["id"] == "design-parity")
            self.assertIn("desktop design-proof", design_ready["command"])
            self.assertIn(str(native_path.resolve()), design_ready["command"])
            self.assertIn(str((repo_root / "planning" / "screenshots" / "reference.png").resolve()), design_ready["command"])
            self.assertNotIn("discovered_report", design_ready)
            self.assertEqual(design_ready["local_readiness"]["status"], "ready")
            design_checks = {check["name"]: check for check in design_ready["local_readiness"]["checks"]}
            self.assertTrue(design_checks["design-parity.native-image"]["ok"])
            self.assertFalse(design_checks["design-parity.run-manifest"]["ok"])
            self.assertFalse(design_checks["design-parity.run-manifest"]["required"])

            published_root = repo_root / "artifact-root" / "_published"
            report_dir = published_root / "20260614-design"
            run_dir = report_dir / "assets" / "run-01"
            run_dir.mkdir(parents=True)
            published_manifest = run_dir / "manifest.json"
            published_source = run_dir / "source-reference" / "figma.png"
            published_source.parent.mkdir(parents=True)
            published_manifest.write_text(json.dumps({"label": "published-design-run"}) + "\n")
            published_source.write_bytes(b"png")
            (report_dir / "index.json").write_text(
                json.dumps(
                    {
                        "label": "published-design-parity",
                        "runs": [
                            {
                                "label": "component",
                                "artifacts": {
                                    "manifest": "assets/run-01/manifest.json",
                                    "video_source_image": "assets/run-01/source-reference/figma.png",
                                },
                                "video_proof_composition": {"template": "design-parity"},
                            }
                        ],
                    }
                )
                + "\n"
            )
            (repo_root / "planning" / "screenshots" / "reference.png").unlink()
            checked_payload = self.mod.desktop_video_matrix_payload(
                target="mac",
                status="ready",
                check=True,
                repo_root=repo_root,
                design_parity_publish_root=published_root,
                which_fn=lambda name: "/usr/bin/cmake" if name == "cmake" else None,
            )
            design_ready = next(item for item in checked_payload["scenarios"] if item["id"] == "design-parity")
            self.assertIn(str(published_manifest.resolve()), design_ready["command"])
            self.assertIn(str(published_source.resolve()), design_ready["command"])
            self.assertEqual(design_ready["local_readiness"]["status"], "ready")
            self.assertEqual(design_ready["discovered_report"]["label"], "published-design-parity")

        self.printed.clear()
        result = self.mod.cmd_desktop_video_matrix(
            Namespace(target="windows", scenario=None, status="ready", json=False, markdown=True, check=False),
            print_fn=self.print_line,
        )
        self.assertEqual(result, 0)
        markdown = self.printed[0]
        self.assertIn("Status filter: `ready` (`declared`)", markdown)
        self.assertIn("No scenarios matched", markdown)
        self.assertIn("Add `--check`", markdown)

    def test_desktop_video_matrix_cli_discovers_published_design_parity_inputs(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            artifact_root = Path(tmpdir) / "artifacts"
            report_dir = artifact_root / "_published" / "20260614-design"
            run_dir = report_dir / "assets" / "run-01"
            run_dir.mkdir(parents=True)
            manifest = run_dir / "manifest.json"
            source = run_dir / "source-reference" / "figma.png"
            source.parent.mkdir(parents=True)
            manifest.write_text(json.dumps({"label": "design-run"}) + "\n")
            source.write_bytes(b"png")
            (report_dir / "index.json").write_text(
                json.dumps(
                    {
                        "runs": [
                            {
                                "label": "design-run",
                                "artifacts": {
                                    "manifest": "assets/run-01/manifest.json",
                                    "video_source_image": "assets/run-01/source-reference/figma.png",
                                },
                                "video_proof_composition": {"template": "design-parity"},
                            }
                        ]
                    }
                )
                + "\n"
            )

            result = self.mod.cmd_desktop_video_matrix(
                Namespace(
                    target="mac",
                    scenario="design-parity",
                    status="ready",
                    json=True,
                    markdown=False,
                    check=True,
                    design_parity_manifest=None,
                    design_parity_source_image=None,
                    design_parity_native_image=None,
                ),
                load_config_fn=lambda: {"desktop_automation": {"artifact_root": str(artifact_root)}},
                print_fn=self.print_line,
            )

        self.assertEqual(result, 0)
        payload = json.loads(self.printed[-1])
        self.assertEqual(payload["scenario_count"], 1)
        row = payload["scenarios"][0]
        self.assertEqual(row["status"], "ready")
        self.assertIn(str(manifest.resolve()), row["command"])
        self.assertIn(str(source.resolve()), row["command"])


if __name__ == "__main__":
    unittest.main()
