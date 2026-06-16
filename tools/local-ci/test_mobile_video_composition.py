#!/usr/bin/env python3
from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest


from module_test_utils import load_local_ci_module


def load_module():
    return load_local_ci_module("mobile_video_composition.py", add_module_dir=True)


class MobileVideoCompositionTests(unittest.TestCase):
    def setUp(self):
        self.mod = load_module()

    def test_compose_mobile_video_proof_updates_manifest_artifacts(self):
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
                        "target": "android-emulator",
                        "action": "video",
                        "label": "android-proof",
                        "artifacts": {"video": str(raw_video)},
                        "video_proof_composition": {
                            "template": "mobile-emulator",
                            "action_marker": {"kind": "open-url", "label": "open deep link"},
                        },
                    }
                )
                + "\n"
            )

            def compose(_manifest_path: Path, output_path: Path, **kwargs):
                self.assertEqual(kwargs["template"], "mobile-emulator")
                self.assertEqual(kwargs["title"], "Android proof")
                self.assertEqual(kwargs["notes"], ["Deep link opened"])
                output_path.write_bytes(b"composed")
                return {"output": str(output_path), "composer": "remotion"}

            def issue(_source_path: Path, output_path: Path, metadata_path: Path, *, attachment_budget_bytes: int, **_kwargs):
                output_path.write_bytes(b"issue")
                payload = {"status": "copied", "output": str(output_path), "budget": attachment_budget_bytes}
                metadata_path.write_text(json.dumps(payload) + "\n")
                return payload

            payload = self.mod.compose_mobile_video_proof(
                manifest_path,
                template="mobile-emulator",
                title="Android proof",
                notes=["Deep link opened"],
                video_attachment_budget_mb=25.0,
                small_video=True,
                small_video_budget_mb=8.0,
                compose_fn=compose,
                issue_variant_fn=issue,
                resolve_ffmpeg_fn=lambda **_kwargs: "/usr/bin/ffmpeg",
            )

            updated = json.loads(manifest_path.read_text())
            self.assertEqual(payload["video_issue"]["budget"], 25_000_000)
            self.assertEqual(payload["video_small"]["budget"], 8_000_000)
            self.assertEqual(updated["video_proof_composition"]["template"], "mobile-emulator")
            self.assertEqual(updated["video_proof_composition"]["action_marker"]["label"], "open deep link")
            self.assertEqual(updated["video_proof_composition"]["title"], "Android proof")
            self.assertEqual(updated["video_proof_composition"]["notes"], ["Deep link opened"])
            self.assertEqual(updated["video_proof_notes"], ["Deep link opened"])
            self.assertTrue(updated["artifacts"]["video_composed"].endswith("/video/proof-composed.mp4"))
            self.assertTrue(updated["artifacts"]["video_issue"].endswith("/video/proof.issue.mp4"))
            self.assertTrue(updated["artifacts"]["video_small"].endswith("/video/proof.small.mp4"))


if __name__ == "__main__":
    unittest.main()
