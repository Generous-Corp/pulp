#!/usr/bin/env python3
"""Tests for the video-proof tool artifact packer."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
import zipfile


MODULE_PATH = Path(__file__).with_name("pack_video_proof_tool.py")


def load_module():
    spec = importlib.util.spec_from_file_location("pack_video_proof_tool_under_test", MODULE_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


class PackVideoProofToolTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = Path(self.tmpdir.name)
        self.package_root = self.root / "tools" / "local-ci"
        (self.package_root / "scripts").mkdir(parents=True)
        (self.package_root / "remotion-proof").mkdir()
        (self.package_root / "node_modules" / "remotion").mkdir(parents=True)
        (self.package_root / ".video-proof-smoke").mkdir()
        (self.package_root / "package.json").write_text(
            json.dumps(
                {
                    "name": "pulp-local-ci-video-tools",
                    "private": True,
                    "version": "0.0.0",
                    "scripts": {"smoke-video-proof": "node scripts/smoke-video-proof.mjs"},
                    "devDependencies": {
                        "remotion": "4.0.476",
                        "ffmpeg-static": "5.2.0",
                    },
                }
            )
            + "\n"
        )
        (self.package_root / "package-lock.json").write_text('{"lockfileVersion":3}\n')
        (self.package_root / "scripts" / "compose-video-proof.mjs").write_text("compose\n")
        (self.package_root / "scripts" / "smoke-video-proof.mjs").write_text("smoke\n")
        (self.package_root / "remotion-proof" / "README.md").write_text("readme\n")
        (self.package_root / "remotion-proof" / "index.jsx").write_text("index\n")
        (self.package_root / "remotion-proof" / "validation-proof.jsx").write_text("proof\n")
        (self.package_root / "node_modules" / "remotion" / "package.json").write_text("{}\n")
        (self.package_root / ".video-proof-smoke" / "proof-composed.mp4").write_bytes(b"video")

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_packer_writes_manifest_and_excludes_generated_dependencies(self) -> None:
        output_dir = self.root / "dist"

        manifest = self.mod.pack_video_proof_tool(
            repo_root=self.root,
            output_dir=output_dir,
            version="0.0.0-test",
        )

        archive_path = Path(manifest["artifact"]["path"])
        manifest_path = Path(manifest["manifest_path"])
        self.assertTrue(archive_path.is_file())
        self.assertTrue(manifest_path.is_file())
        self.assertEqual(manifest["tool_id"], "video-proof")
        self.assertEqual(manifest["distribution_lane"], "tool_addon")
        self.assertEqual(manifest["package_format"], "not_pulp_add")
        self.assertEqual(manifest["policy"]["machine_scoped_tool"], True)
        self.assertEqual(manifest["policy"]["bundles_node_modules"], False)
        self.assertEqual(manifest["policy"]["bundles_generated_videos"], False)
        self.assertEqual(manifest["npm_package"]["dev_dependencies"]["remotion"], "4.0.476")
        self.assertEqual(manifest["npm_package"]["dev_dependencies"]["ffmpeg-static"], "5.2.0")

        with zipfile.ZipFile(archive_path) as archive:
            names = sorted(archive.namelist())

        self.assertIn("tools/local-ci/package.json", names)
        self.assertIn("tools/local-ci/package-lock.json", names)
        self.assertIn("tools/local-ci/scripts/compose-video-proof.mjs", names)
        self.assertIn("tools/local-ci/remotion-proof/validation-proof.jsx", names)
        self.assertFalse(any("node_modules" in name for name in names))
        self.assertFalse(any(".video-proof-smoke" in name for name in names))
        self.assertEqual(names, manifest["included_files"])
        self.assertEqual(json.loads(manifest_path.read_text())["artifact"]["sha256"], manifest["artifact"]["sha256"])

    def test_packer_fails_when_required_payload_file_is_missing(self) -> None:
        (self.package_root / "scripts" / "compose-video-proof.mjs").unlink()

        with self.assertRaisesRegex(RuntimeError, "missing: scripts/compose-video-proof.mjs"):
            self.mod.pack_video_proof_tool(repo_root=self.root, output_dir=self.root / "dist")


if __name__ == "__main__":
    unittest.main()
