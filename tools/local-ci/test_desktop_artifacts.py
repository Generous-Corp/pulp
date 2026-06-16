#!/usr/bin/env python3
"""No-network tests for local-ci desktop artifact path helpers."""

from __future__ import annotations

from datetime import datetime
import json
import pathlib
import tempfile
import unittest

from module_test_utils import load_local_ci_module



def load_module():
    return load_local_ci_module("desktop_artifacts.py")


class DesktopArtifactsTests(unittest.TestCase):
    def setUp(self) -> None:
        self.mod = load_module()
        self.tmpdir = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.tmpdir.name)
        self.config = {"desktop_automation": {"artifact_root": str(self.root / "desktop-artifacts")}}

    def tearDown(self) -> None:
        self.tmpdir.cleanup()

    def test_desktop_artifact_root_expands_and_creates_directory(self) -> None:
        root = self.mod.desktop_artifact_root(self.config)

        self.assertEqual(root, self.root / "desktop-artifacts")
        self.assertTrue(root.is_dir())

    def test_desktop_receipt_helpers_read_target_receipts(self) -> None:
        receipts_dir = self.root / "receipts"
        receipt_path = self.mod.desktop_target_receipt_path(
            "windows",
            desktop_receipts_dir_fn=lambda: receipts_dir,
        )
        self.assertEqual(receipt_path, receipts_dir / "windows.json")
        self.assertIsNone(
            self.mod.desktop_receipt_for(
                "windows",
                desktop_target_receipt_path_fn=lambda name: receipts_dir / f"{name}.json",
            )
        )

        receipts_dir.mkdir()
        receipt_path.write_text(json.dumps({"target": "windows", "ready": True}) + "\n")

        self.assertEqual(
            self.mod.desktop_receipt_for(
                "windows",
                desktop_target_receipt_path_fn=lambda name: receipts_dir / f"{name}.json",
            ),
            {"target": "windows", "ready": True},
        )

    def test_create_desktop_run_bundle_uses_stable_layout(self) -> None:
        bundle = self.mod.create_desktop_run_bundle(
            self.config,
            "mac",
            "smoke",
            now_fn=lambda: datetime(2026, 6, 9, 12, 0, 0),
            uuid_hex_fn=lambda: "abcdef1234567890",
        )

        self.assertEqual(
            bundle,
            self.root / "desktop-artifacts" / "mac" / "smoke" / "20260609-120000-abcdef12",
        )
        self.assertTrue((bundle / "screenshots").is_dir())

    def test_create_desktop_publish_bundle_uses_stable_layout(self) -> None:
        bundle = self.mod.create_desktop_publish_bundle(
            self.config,
            now_fn=lambda: datetime(2026, 6, 9, 12, 0, 0),
            uuid_hex_fn=lambda: "fedcba9876543210",
        )

        self.assertEqual(
            bundle,
            self.root / "desktop-artifacts" / "_published" / "20260609-120000-fedcba98",
        )
        self.assertTrue((bundle / "assets").is_dir())


if __name__ == "__main__":
    unittest.main()
