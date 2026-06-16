#!/usr/bin/env python3
"""Tests for the desktop compose-video / design-diff / design-proof commands."""

from argparse import Namespace
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import tempfile
import unittest

from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("desktop_video_compose_commands_cli.py")


class DesktopVideoComposeCommandsTests(unittest.TestCase):
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
                "targets": {
                    "mac": {
                        "enabled": True,
                        "adapter": "macos-local",
                        "bootstrap": "launchagent",
                        "target_type": "local",
                        "capability_tier": "full",
                        "optional": {"webview_driver": True},
                    }
                },
            }
        }

    def test_compose_video_updates_manifest_artifacts(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            run_dir = Path(tmpdir) / "run"
            video_dir = run_dir / "video"
            video_dir.mkdir(parents=True)
            raw_video = video_dir / "proof.mp4"
            raw_video.write_bytes(b"raw")
            manifest_path = run_dir / "manifest.json"
            manifest_path.write_text(
                json.dumps(
                    {
                        "label": "video-proof",
                        "artifacts": {
                            "video": str(raw_video),
                        },
                        "video_proof_composition": {
                            "template": "component-zoom",
                            "focus": {"label": "bypass-toggle", "selector": {"click_view_id": "bypass-toggle"}},
                        },
                    }
                )
                + "\n"
            )
            (run_dir / "reference.png").write_bytes(b"png")
            (run_dir / "diff.png").write_bytes(b"diff")
            writes = []

            def compose(manifest_path_arg: Path, output_path: Path, **kwargs):
                self.assertEqual(kwargs["template"], "design-parity")
                self.assertEqual(kwargs["source_image"], (run_dir / "reference.png").resolve())
                self.assertEqual(kwargs["source_label"], "Figma reference")
                self.assertEqual(kwargs["diff_image"], (run_dir / "diff.png").resolve())
                self.assertEqual(kwargs["diff_label"], "Delta heatmap")
                self.assertEqual(kwargs["title"], "Design parity")
                self.assertEqual(kwargs["notes"], ["Reference matches implementation"])
                output_path.write_bytes(b"composed")
                return {"output": str(output_path), "composer": "remotion"}

            issue_calls = []

            def issue(source_path: Path, output_path: Path, metadata_path: Path, *, attachment_budget_bytes: int):
                self.assertEqual(source_path, (video_dir / "proof-composed.mp4").resolve())
                issue_calls.append((output_path, metadata_path, attachment_budget_bytes))
                output_path.write_bytes(b"issue")
                payload = {"status": "copied", "output": str(output_path), "budget": attachment_budget_bytes}
                metadata_path.write_text(json.dumps(payload) + "\n")
                return payload

            result = self.mod.cmd_desktop_compose_video(
                Namespace(
                    manifest=str(manifest_path),
                    output=None,
                    metadata=None,
                    issue_output=None,
                    issue_metadata=None,
                    small_video=True,
                    small_output=None,
                    small_metadata=None,
                    small_video_budget_mb=10.0,
                    template="design-parity",
                    source_image=str(run_dir / "reference.png"),
                    source_label="Figma reference",
                    diff_image=str(run_dir / "diff.png"),
                    diff_label="Delta heatmap",
                    title="Design parity",
                    note=["Reference matches implementation"],
                    video_attachment_budget_mb=40.0,
                    json=True,
                ),
                compose_desktop_video_proof_fn=compose,
                create_issue_video_variant_fn=issue,
                atomic_write_text_fn=lambda path, text: writes.append((path, text)) or path.write_text(text),
                print_fn=self.print_line,
            )

            self.assertEqual(result, 0)
            payload = json.loads(self.printed[-1])
            self.assertEqual(payload["video_issue"]["status"], "copied")
            self.assertEqual(payload["video_issue"]["budget"], 40_000_000)
            self.assertEqual(payload["video_small"]["budget"], 10_000_000)
            self.assertEqual([call[2] for call in issue_calls], [40_000_000, 10_000_000])
            updated = json.loads(manifest_path.read_text())
            self.assertEqual(updated["video_composed"]["composer"], "remotion")
            self.assertEqual(updated["video_small"]["budget"], 10_000_000)
            self.assertEqual(updated["video_proof_composition"]["template"], "design-parity")
            self.assertTrue(updated["video_proof_composition"]["source_image"].endswith("/reference.png"))
            self.assertEqual(updated["video_proof_composition"]["source_label"], "Figma reference")
            self.assertTrue(updated["video_proof_composition"]["diff_image"].endswith("/diff.png"))
            self.assertEqual(updated["video_proof_composition"]["diff_label"], "Delta heatmap")
            self.assertEqual(updated["video_proof_composition"]["title"], "Design parity")
            self.assertEqual(updated["video_proof_composition"]["notes"], ["Reference matches implementation"])
            self.assertEqual(updated["video_proof_composition"]["focus"]["label"], "bypass-toggle")
            self.assertEqual(updated["video_proof_notes"], ["Reference matches implementation"])
            self.assertTrue(updated["artifacts"]["video_composed"].endswith("/video/proof-composed.mp4"))
            self.assertTrue(updated["artifacts"]["video_composed_metadata"].endswith("/video/composed-metadata.json"))
            self.assertTrue(updated["artifacts"]["video_issue"].endswith("/video/proof.issue.mp4"))
            self.assertTrue(updated["artifacts"]["video_issue_metadata"].endswith("/video/issue-metadata.json"))
            self.assertTrue(updated["artifacts"]["video_small"].endswith("/video/proof.small.mp4"))
            self.assertTrue(updated["artifacts"]["video_small_metadata"].endswith("/video/small-metadata.json"))
            self.assertIn(manifest_path.resolve(), [path for path, _text in writes])

    def test_compose_video_uses_existing_diff_screenshot_for_design_parity(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            run_dir = Path(tmpdir) / "run"
            video_dir = run_dir / "video"
            video_dir.mkdir(parents=True)
            raw_video = video_dir / "proof.mp4"
            raw_video.write_bytes(b"raw")
            diff = run_dir / "screenshots" / "diff.png"
            diff.parent.mkdir()
            diff.write_bytes(b"diff")
            manifest_path = run_dir / "manifest.json"
            manifest_path.write_text(
                json.dumps(
                    {
                        "label": "design-proof",
                        "artifacts": {
                            "video": str(raw_video),
                            "diff_screenshot": str(diff),
                        },
                    }
                )
                + "\n"
            )

            def compose(_manifest_path: Path, output_path: Path, **kwargs):
                self.assertEqual(kwargs["template"], "design-parity")
                self.assertEqual(kwargs["diff_image"], diff.resolve())
                output_path.write_bytes(b"composed")
                return {"output": str(output_path), "composer": "remotion"}

            def issue(_source_path: Path, output_path: Path, metadata_path: Path, *, attachment_budget_bytes: int):
                output_path.write_bytes(b"issue")
                payload = {"status": "copied", "output": str(output_path), "budget": attachment_budget_bytes}
                metadata_path.write_text(json.dumps(payload) + "\n")
                return payload

            result = self.mod.cmd_desktop_compose_video(
                Namespace(
                    manifest=str(manifest_path),
                    output=None,
                    metadata=None,
                    issue_output=None,
                    issue_metadata=None,
                    small_video=False,
                    small_output=None,
                    small_metadata=None,
                    small_video_budget_mb=10.0,
                    template="design-parity",
                    source_image=None,
                    source_label=None,
                    diff_image=None,
                    diff_label=None,
                    title=None,
                    note=[],
                    video_attachment_budget_mb=40.0,
                    json=True,
                ),
                compose_desktop_video_proof_fn=compose,
                create_issue_video_variant_fn=issue,
                atomic_write_text_fn=lambda path, text: path.write_text(text),
                print_fn=self.print_line,
            )

            self.assertEqual(result, 0)
            updated = json.loads(manifest_path.read_text())
            self.assertEqual(updated["video_proof_composition"]["template"], "design-parity")
            self.assertTrue(updated["video_proof_composition"]["diff_image"].endswith("/screenshots/diff.png"))

    def test_compose_video_accepts_mobile_simulator_template(self):
        with tempfile.TemporaryDirectory() as tmp:
            run_dir = Path(tmp)
            video_dir = run_dir / "video"
            video_dir.mkdir()
            raw_video = video_dir / "proof.mp4"
            raw_video.write_bytes(b"raw")
            manifest_path = run_dir / "manifest.json"
            manifest_path.write_text(
                json.dumps(
                    {
                        "target": "ios-simulator",
                        "action": "video",
                        "label": "ios-simulator-openurl",
                        "interaction": {"mode": "open-url", "label": "open example.com"},
                        "artifacts": {"video": str(raw_video)},
                        "video_proof_composition": {
                            "template": "mobile-simulator",
                            "action_marker": {"kind": "open-url", "label": "open example.com"},
                        },
                    }
                )
                + "\n"
            )

            def compose(_manifest_path: Path, output_path: Path, **kwargs):
                self.assertEqual(kwargs["template"], "mobile-simulator")
                self.assertEqual(kwargs["title"], "Simulator proof")
                self.assertEqual(kwargs["notes"], ["URL opened during capture"])
                output_path.write_bytes(b"composed")
                return {"output": str(output_path), "composer": "remotion"}

            def issue(_source_path: Path, output_path: Path, metadata_path: Path, *, attachment_budget_bytes: int):
                output_path.write_bytes(b"issue")
                payload = {"status": "copied", "output": str(output_path), "budget": attachment_budget_bytes}
                metadata_path.write_text(json.dumps(payload) + "\n")
                return payload

            result = self.mod.cmd_desktop_compose_video(
                Namespace(
                    manifest=str(manifest_path),
                    output=None,
                    metadata=None,
                    issue_output=None,
                    issue_metadata=None,
                    small_video=True,
                    small_output=None,
                    small_metadata=None,
                    small_video_budget_mb=10.0,
                    template="mobile-simulator",
                    source_image=None,
                    source_label=None,
                    title="Simulator proof",
                    note=["URL opened during capture"],
                    video_attachment_budget_mb=100.0,
                    json=True,
                ),
                compose_desktop_video_proof_fn=compose,
                create_issue_video_variant_fn=issue,
                atomic_write_text_fn=lambda path, text: path.write_text(text),
                print_fn=self.print_line,
            )

            self.assertEqual(result, 0)
            updated = json.loads(manifest_path.read_text())
            self.assertEqual(updated["video_proof_composition"]["template"], "mobile-simulator")
            self.assertEqual(updated["video_proof_composition"]["action_marker"]["label"], "open example.com")
            self.assertEqual(updated["video_proof_composition"]["title"], "Simulator proof")
            self.assertEqual(updated["video_proof_composition"]["notes"], ["URL opened during capture"])
            self.assertTrue(updated["artifacts"]["video_composed"].endswith("/video/proof-composed.mp4"))
            self.assertTrue(updated["artifacts"]["video_issue"].endswith("/video/proof.issue.mp4"))
            self.assertTrue(updated["artifacts"]["video_small"].endswith("/video/proof.small.mp4"))

    def test_design_diff_uses_manifest_screenshot_and_writes_metadata(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            source = root / "source.png"
            source.write_bytes(b"source")
            screenshot = root / "window.png"
            screenshot.write_bytes(b"native")
            manifest_path = root / "manifest.json"
            manifest_path.write_text(json.dumps({"artifacts": {"screenshot": str(screenshot)}}) + "\n")
            output = root / "diff" / "source-native.png"
            resized = root / "diff" / "source-resized.png"
            metadata = root / "diff" / "metadata.json"

            def design_diff(source_path: Path, native_path: Path, **kwargs):
                self.assertEqual(source_path, source.resolve())
                self.assertEqual(native_path, screenshot.resolve())
                self.assertEqual(kwargs["diff_output_path"], output.resolve())
                self.assertEqual(kwargs["resized_source_output_path"], resized.resolve())
                self.assertEqual(kwargs["enhance_brightness"], 2.5)
                output.parent.mkdir(parents=True, exist_ok=True)
                output.write_bytes(b"diff")
                resized.write_bytes(b"resized")
                return {
                    "diff_image": str(output.resolve()),
                    "resized_source_image": str(resized.resolve()),
                    "changed": True,
                    "bbox": {"left": 0, "top": 1, "right": 2, "bottom": 3},
                }

            result = self.mod.cmd_desktop_design_diff(
                Namespace(
                    source_image=str(source),
                    native_image=None,
                    manifest=str(manifest_path),
                    output=str(output),
                    resized_source_output=str(resized),
                    enhance_brightness=2.5,
                    metadata=str(metadata),
                    json=True,
                ),
                design_parity_diff_summary_fn=design_diff,
                atomic_write_text_fn=lambda path, text: path.parent.mkdir(parents=True, exist_ok=True) or path.write_text(text),
                print_fn=self.print_line,
            )

            self.assertEqual(result, 0)
            payload = json.loads(self.printed[-1])
            self.assertEqual(payload["kind"], "desktop-design-parity-diff")
            self.assertEqual(payload["diff_image"], str(output.resolve()))
            self.assertEqual(payload["metadata"], str(metadata.resolve()))
            self.assertEqual(payload["summary"]["bbox"]["right"], 2)
            self.assertEqual(payload["compose_args"], ["--diff-image", str(output.resolve()), "--diff-label", "Source vs native screenshot diff"])
            written = json.loads(metadata.read_text())
            self.assertEqual(written["native_image"], str(screenshot.resolve()))

    def test_design_proof_creates_manifest_diff_and_composed_variants(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            root = Path(tmpdir)
            source = root / "source.png"
            native = root / "native.png"
            source.write_bytes(b"source")
            native.write_bytes(b"native")
            run_dir = root / "run"
            resolved_run_dir = run_dir.resolve()

            def design_diff(source_path: Path, native_path: Path, **kwargs):
                self.assertEqual(source_path, source.resolve())
                self.assertEqual(native_path, native.resolve())
                self.assertEqual(kwargs["diff_output_path"], resolved_run_dir / "video" / "source-vs-native-diff.png")
                self.assertEqual(kwargs["resized_source_output_path"], resolved_run_dir / "video" / "source-resized-to-native.png")
                self.assertEqual(kwargs["enhance_brightness"], 2.0)
                kwargs["diff_output_path"].write_bytes(b"diff")
                kwargs["resized_source_output_path"].write_bytes(b"resized")
                return {
                    "changed": True,
                    "bbox": {"left": 0, "top": 0, "right": 10, "bottom": 10},
                }

            def compose(manifest_path_arg: Path, output_path: Path, **kwargs):
                self.assertEqual(manifest_path_arg, resolved_run_dir / "manifest.json")
                self.assertEqual(kwargs["template"], "design-parity")
                self.assertEqual(kwargs["source_image"], source.resolve())
                self.assertEqual(kwargs["source_label"], "Figma REST reference")
                self.assertEqual(kwargs["diff_image"], resolved_run_dir / "video" / "source-vs-native-diff.png")
                self.assertEqual(kwargs["diff_label"], "Delta heatmap")
                self.assertEqual(kwargs["title"], "ELYSIUM proof")
                self.assertEqual(kwargs["notes"], ["Reference frozen", "Known ROI gap"])
                output_path.write_bytes(b"composed")
                return {"output": str(output_path), "composer": "remotion"}

            def issue(source_path: Path, output_path: Path, metadata_path: Path, *, attachment_budget_bytes: int):
                self.assertEqual(source_path, resolved_run_dir / "video" / "proof-composed.mp4")
                output_path.write_bytes(b"issue")
                payload = {"status": "copied", "output": str(output_path), "budget": attachment_budget_bytes}
                metadata_path.write_text(json.dumps(payload) + "\n")
                return payload

            result = self.mod.cmd_desktop_design_proof(
                Namespace(
                    source_image=str(source),
                    native_image=str(native),
                    output_dir=str(run_dir),
                    label="elysium-proof",
                    source_label="Figma REST reference",
                    diff_label="Delta heatmap",
                    title="ELYSIUM proof",
                    note=["Reference frozen", "Known ROI gap"],
                    context=["fixture=elysium.pulp.zip", "native=mac gpu harness"],
                    enhance_brightness=2.0,
                    video_attachment_budget_mb=40.0,
                    small_video=True,
                    small_video_budget_mb=10.0,
                    json=True,
                ),
                load_config_fn=self.desktop_config,
                design_parity_diff_summary_fn=design_diff,
                compose_desktop_video_proof_fn=compose,
                create_issue_video_variant_fn=issue,
                atomic_write_text_fn=lambda path, text: path.parent.mkdir(parents=True, exist_ok=True) or path.write_text(text),
                now_fn=lambda: datetime(2026, 6, 14, 15, 0, 0, tzinfo=timezone.utc),
                print_fn=self.print_line,
            )

            self.assertEqual(result, 0)
            payload = json.loads(self.printed[-1])
            self.assertEqual(payload["kind"], "desktop-design-proof")
            self.assertEqual(payload["manifest"], str(resolved_run_dir / "manifest.json"))
            self.assertEqual(payload["video_issue"]["budget"], 40_000_000)
            self.assertEqual(payload["video_small"]["budget"], 10_000_000)
            self.assertEqual(payload["diff"]["summary"]["bbox"]["right"], 10)
            manifest = json.loads((run_dir / "manifest.json").read_text())
            self.assertEqual(manifest["target"], "mac")
            self.assertEqual(manifest["action"], "design-parity")
            self.assertEqual(manifest["label"], "elysium-proof")
            self.assertEqual(manifest["source"]["mode"], "still-images")
            self.assertEqual(manifest["video_proof_composition"]["context"]["fixture"], "elysium.pulp.zip")
            self.assertEqual(manifest["video_proof_composition"]["context"]["native"], "mac gpu harness")
            self.assertTrue(manifest["artifacts"]["diff_screenshot"].endswith("/video/source-vs-native-diff.png"))
            self.assertTrue(manifest["artifacts"]["video_composed"].endswith("/video/proof-composed.mp4"))
            self.assertTrue(manifest["artifacts"]["video_small"].endswith("/video/proof.small.mp4"))

    def test_design_diff_requires_native_image_or_manifest_screenshot(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            source = Path(tmpdir) / "source.png"
            source.write_bytes(b"source")
            result = self.mod.cmd_desktop_design_diff(
                Namespace(
                    source_image=str(source),
                    native_image=None,
                    manifest=None,
                    output=str(Path(tmpdir) / "diff.png"),
                    resized_source_output=None,
                    enhance_brightness=3.0,
                    metadata=None,
                    json=False,
                ),
                design_parity_diff_summary_fn=lambda *_args, **_kwargs: self.fail("diff should not run"),
                atomic_write_text_fn=lambda _path, _text: None,
                print_fn=self.print_line,
            )

            self.assertEqual(result, 1)
            self.assertIn("native image required", self.printed[-1])

    def test_compose_video_reports_missing_manifest(self):
        result = self.mod.cmd_desktop_compose_video(
            Namespace(
                manifest="/tmp/does-not-exist/manifest.json",
                output=None,
                metadata=None,
                issue_output=None,
                issue_metadata=None,
                template=None,
                source_image=None,
                source_label=None,
                title=None,
                video_attachment_budget_mb=100.0,
                json=False,
            ),
            compose_desktop_video_proof_fn=lambda *_args, **_kwargs: {},
            create_issue_video_variant_fn=lambda *_args, **_kwargs: {},
            atomic_write_text_fn=lambda path, text: path.write_text(text),
            print_fn=self.print_line,
        )

        self.assertEqual(result, 1)
        self.assertIn("manifest not found", self.printed[-1])

    def test_compose_video_rejects_missing_source_image(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            manifest_path = Path(tmpdir) / "manifest.json"
            manifest_path.write_text('{"label":"video-proof","artifacts":{}}\n')
            result = self.mod.cmd_desktop_compose_video(
                Namespace(
                    manifest=str(manifest_path),
                    output=None,
                    metadata=None,
                    issue_output=None,
                    issue_metadata=None,
                    template="design-parity",
                    source_image=str(Path(tmpdir) / "missing.png"),
                    source_label=None,
                    title=None,
                    video_attachment_budget_mb=40.0,
                    json=False,
                ),
                compose_desktop_video_proof_fn=lambda *_args, **_kwargs: self.fail("compose should not run"),
                create_issue_video_variant_fn=lambda *_args, **_kwargs: self.fail("issue variant should not run"),
                atomic_write_text_fn=lambda path, text: path.write_text(text),
                print_fn=self.print_line,
            )

        self.assertEqual(result, 1)
        self.assertIn("source image not found", self.printed[-1])



if __name__ == "__main__":
    unittest.main()
