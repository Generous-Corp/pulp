#!/usr/bin/env python3
"""No-network tests for local-ci video artifact helpers."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest


MODULE_PATH = Path(__file__).with_name("video_artifacts.py")


def load_module():
    spec = importlib.util.spec_from_file_location("video_artifacts_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class VideoArtifactsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_resolve_ffmpeg_path_prefers_explicit_env(self) -> None:
        ffmpeg = self.root / "ffmpeg"
        fallback = self.root / "ffmpeg-path"
        ffmpeg.write_text("#!/bin/sh\n")
        fallback.write_text("#!/bin/sh\n")

        self.assertEqual(
            self.mod.resolve_ffmpeg_path(
                env={"PULP_FFMPEG": str(ffmpeg), "PULP_FFMPEG_PATH": str(fallback)},
                which_fn=lambda _name: None,
            ),
            str(ffmpeg),
        )

        self.assertEqual(
            self.mod.resolve_ffmpeg_path(env={"PULP_FFMPEG_PATH": str(fallback)}, which_fn=lambda _name: None),
            str(fallback),
        )

    def test_resolve_ffmpeg_path_uses_path_then_local_static_package(self) -> None:
        self.assertEqual(
            self.mod.resolve_ffmpeg_path(env={}, which_fn=lambda name: "/usr/bin/ffmpeg" if name == "ffmpeg" else None),
            "/usr/bin/ffmpeg",
        )

        local_ffmpeg = self.root / "node_modules" / "ffmpeg-static" / "ffmpeg"
        local_ffmpeg.parent.mkdir(parents=True)
        local_ffmpeg.write_text("#!/bin/sh\n")

        self.assertEqual(
            self.mod.resolve_ffmpeg_path(env={}, which_fn=lambda _name: None, tool_dir=self.root),
            str(local_ffmpeg),
        )

    def test_resolve_ffmpeg_path_reports_setup_hint(self) -> None:
        with self.assertRaisesRegex(RuntimeError, "npm --prefix tools/local-ci install"):
            self.mod.resolve_ffmpeg_path(env={}, which_fn=lambda _name: None, tool_dir=self.root)

    def test_desktop_video_metadata_records_audio_source_and_encoder(self) -> None:
        proof = self.root / "proof.mp4"
        proof.write_bytes(b"mp4")

        payload = self.mod.desktop_video_metadata(
            proof,
            duration_secs=2.0,
            fps=24.0,
            audio_source="none",
            encoder={"path": "/opt/ffmpeg", "version": "ffmpeg version 6.0"},
        )

        self.assertFalse(payload["has_audio"])
        self.assertEqual(payload["audio_source"], "none")
        self.assertEqual(payload["encoder"]["path"], "/opt/ffmpeg")
        self.assertEqual(payload["encoder"]["version"], "ffmpeg version 6.0")

    def test_compose_desktop_video_proof_runs_remotion_script(self) -> None:
        manifest = self.root / "manifest.json"
        output = self.root / "proof-composed.mp4"
        script = self.root / "compose.mjs"
        source = self.root / "source.png"
        manifest.write_text('{"label":"demo"}\n')
        script.write_text("")
        source.write_bytes(b"png")
        calls = []

        def run_compose(cmd, **kwargs):
            calls.append((cmd, kwargs))
            output.write_bytes(b"mp4")
            return self.mod.subprocess.CompletedProcess(cmd, 0, stdout='{"composer":"remotion"}\n', stderr="")

        payload = self.mod.compose_desktop_video_proof(
            manifest,
            output,
            script_path=script,
            template="design-parity",
            source_image=source,
            source_label="Figma reference",
            title="Design parity",
            run_fn=run_compose,
        )

        self.assertEqual(calls[0][0][0], "node")
        self.assertIn("--manifest", calls[0][0])
        self.assertEqual(calls[0][0][calls[0][0].index("--template") + 1], "design-parity")
        self.assertEqual(calls[0][0][calls[0][0].index("--source-image") + 1], str(source))
        self.assertEqual(calls[0][0][calls[0][0].index("--source-label") + 1], "Figma reference")
        self.assertEqual(calls[0][0][calls[0][0].index("--title") + 1], "Design parity")
        self.assertEqual(payload["composer"], "remotion")
        self.assertTrue(payload["size"]["fits_attachment_budget"])

    def test_create_issue_video_variant_copies_source_when_it_fits(self) -> None:
        source = self.root / "proof-composed.mp4"
        output = self.root / "proof.issue.mp4"
        metadata = self.root / "issue-metadata.json"
        source.write_bytes(b"small-video")

        payload = self.mod.create_issue_video_variant(
            source,
            output,
            metadata,
            attachment_budget_bytes=100,
            ffmpeg_path="/opt/ffmpeg",
            run_fn=lambda *_args, **_kwargs: self.fail("ffmpeg should not run when source fits"),
        )

        self.assertEqual(payload["status"], "copied")
        self.assertEqual(output.read_bytes(), b"small-video")
        self.assertTrue(payload["size"]["fits_attachment_budget"])
        self.assertEqual(payload, self.mod.json.loads(metadata.read_text()))

    def test_create_issue_video_variant_transcodes_when_source_exceeds_budget(self) -> None:
        source = self.root / "proof-composed.mp4"
        output = self.root / "proof.issue.mp4"
        metadata = self.root / "issue-metadata.json"
        source.write_bytes(b"x" * 200)
        calls = []

        def run_transcode(cmd, **kwargs):
            calls.append((cmd, kwargs))
            output.write_bytes(b"small")
            return self.mod.subprocess.CompletedProcess(cmd, 0, stdout="", stderr="encoded")

        payload = self.mod.create_issue_video_variant(
            source,
            output,
            metadata,
            attachment_budget_bytes=100,
            ffmpeg_path="/opt/ffmpeg",
            run_fn=run_transcode,
        )

        self.assertEqual(payload["status"], "transcoded")
        self.assertEqual(payload["selected_attempt"], "balanced-720p")
        self.assertEqual(calls[0][0][0], "/opt/ffmpeg")
        self.assertIn("-crf", calls[0][0])
        self.assertEqual(len(payload["attempts"]), 1)
        self.assertTrue(payload["size"]["fits_attachment_budget"])

    def test_create_issue_video_variant_retries_until_transcode_fits(self) -> None:
        source = self.root / "proof-composed.mp4"
        output = self.root / "proof.issue.mp4"
        metadata = self.root / "issue-metadata.json"
        source.write_bytes(b"x" * 200)
        sizes = [150, 80]

        def run_transcode(cmd, **_kwargs):
            output.write_bytes(b"y" * sizes.pop(0))
            return self.mod.subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

        payload = self.mod.create_issue_video_variant(
            source,
            output,
            metadata,
            attachment_budget_bytes=100,
            ffmpeg_path="/opt/ffmpeg",
            run_fn=run_transcode,
        )

        self.assertEqual(payload["status"], "transcoded")
        self.assertEqual(payload["selected_attempt"], "compact-720p")
        self.assertEqual([attempt["status"] for attempt in payload["attempts"]], ["exceeds-budget", "transcoded"])
        self.assertTrue(payload["size"]["fits_attachment_budget"])

    def test_create_issue_video_variant_reports_oversized_transcode_after_ladder(self) -> None:
        source = self.root / "proof-composed.mp4"
        output = self.root / "proof.issue.mp4"
        metadata = self.root / "issue-metadata.json"
        source.write_bytes(b"x" * 200)

        def run_transcode(cmd, **_kwargs):
            output.write_bytes(b"y" * 150)
            return self.mod.subprocess.CompletedProcess(cmd, 0, stdout="", stderr="")

        payload = self.mod.create_issue_video_variant(
            source,
            output,
            metadata,
            attachment_budget_bytes=100,
            ffmpeg_path="/opt/ffmpeg",
            run_fn=run_transcode,
        )

        self.assertEqual(payload["status"], "exceeds-budget")
        self.assertEqual(payload["selected_attempt"], "compact-540p")
        self.assertEqual(len(payload["attempts"]), len(self.mod.ISSUE_VIDEO_TRANSCODE_ATTEMPTS))
        self.assertTrue(all(attempt["status"] == "exceeds-budget" for attempt in payload["attempts"]))
        self.assertFalse(payload["size"]["fits_attachment_budget"])


if __name__ == "__main__":
    unittest.main()
